#include <ydb/core/tx/columnshard/columnshard_impl.h>
#include <ydb/core/tx/columnshard/columnshard_private_events.h>
#include <ydb/core/tx/columnshard/columnshard_schema.h>
#include <ydb/core/tx/columnshard/columnshard__read_base.h>
#include <ydb/core/tx/columnshard/columnshard__index_scan.h>
#include <ydb/core/tx/columnshard/engines/indexed_read_data.h>

namespace NKikimr::NColumnShard {

namespace {

template <typename T, typename U>
TVector<T> ProtoToVector(const U& cont) {
    return TVector<T>(cont.begin(), cont.end());
}

}

using namespace NTabletFlatExecutor;

class TTxRead : public TTxReadBase {
public:
    TTxRead(TColumnShard* self, TEvColumnShard::TEvRead::TPtr& ev)
        : TTxReadBase(self)
        , Ev(ev)
    {}

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override;
    void Complete(const TActorContext& ctx) override;
    TTxType GetTxType() const override { return TXTYPE_READ; }

private:
    TEvColumnShard::TEvRead::TPtr Ev;
    std::unique_ptr<TEvColumnShard::TEvReadResult> Result;
    NOlap::TReadMetadata::TConstPtr ReadMetadata;
};


bool TTxRead::Execute(TTransactionContext& txc, const TActorContext& ctx) {
    Y_VERIFY(Ev);
    Y_VERIFY(Self->PrimaryIndex);
    Y_UNUSED(txc);
    LOG_S_DEBUG("TTxRead.Execute at tablet " << Self->TabletID());

    txc.DB.NoMoreReadsForTx();

    const NOlap::TIndexInfo& indexInfo = Self->PrimaryIndex->GetIndexInfo();
    auto& record = Proto(Ev->Get());

    ui64 metaShard = record.GetTxInitiator();

    TReadDescription read;
    read.PlanStep = record.GetPlanStep();
    read.TxId = record.GetTxId();
    read.PathId = record.GetTableId();
    read.ReadNothing = Self->PathsToDrop.count(read.PathId);
    read.ColumnIds = ProtoToVector<ui32>(record.GetColumnIds());
    read.ColumnNames = ProtoToVector<TString>(record.GetColumnNames());
    if (read.ColumnIds.empty() && read.ColumnNames.empty()) {
        auto allColumnNames = indexInfo.ArrowSchema()->field_names();
        read.ColumnNames.assign(allColumnNames.begin(), allColumnNames.end());
    }

    if (record.HasGreaterPredicate()) {
        auto& proto = record.GetGreaterPredicate();
        auto schema = indexInfo.ArrowSchema(ProtoToVector<TString>(proto.GetColumnNames()));
        read.GreaterPredicate = std::make_shared<NOlap::TPredicate>(
            NArrow::EOperation::Greater, proto.GetRow(), schema, proto.GetInclusive());
    }
    if (record.HasLessPredicate()) {
        auto& proto = record.GetLessPredicate();
        auto schema = indexInfo.ArrowSchema(ProtoToVector<TString>(proto.GetColumnNames()));
        read.LessPredicate = std::make_shared<NOlap::TPredicate>(
            NArrow::EOperation::Less, proto.GetRow(), schema, proto.GetInclusive());
    }

    bool parseResult = ParseProgram(ctx, record.GetOlapProgramType(), record.GetOlapProgram(), read,
        TIndexColumnResolver(Self->PrimaryIndex->GetIndexInfo()));

    std::shared_ptr<NOlap::TReadMetadata> metadata;
    if (parseResult) {
        metadata = PrepareReadMetadata(ctx, read, Self->InsertTable, Self->PrimaryIndex, Self->BatchCache,
                                       ErrorDescription);
    }

    ui32 status = NKikimrTxColumnShard::EResultStatus::ERROR;

    if (metadata) {
        Self->MapExternBlobs(ctx, *metadata);
        ReadMetadata = metadata;
        status = NKikimrTxColumnShard::EResultStatus::SUCCESS;
    }

    Result = std::make_unique<TEvColumnShard::TEvReadResult>(
        Self->TabletID(), metaShard, read.PlanStep, read.TxId, read.PathId, 0, true, status);

    if (status == NKikimrTxColumnShard::EResultStatus::SUCCESS) {
        Self->IncCounter(COUNTER_READ_SUCCESS);
    } else {
        Self->IncCounter(COUNTER_READ_FAIL);
    }
    return true;
}

void TTxRead::Complete(const TActorContext& ctx) {
    Y_VERIFY(Ev);
    Y_VERIFY(Result);

    bool noData = !ReadMetadata || ReadMetadata->Empty();
    bool success = (Proto(Result.get()).GetStatus() == NKikimrTxColumnShard::EResultStatus::SUCCESS);

    if (!success) {
        LOG_S_DEBUG("TTxRead.Complete. Error " << ErrorDescription << " while reading at tablet " << Self->TabletID());
        ctx.Send(Ev->Get()->GetSource(), Result.release());
    } else if (noData) {
        LOG_S_DEBUG("TTxRead.Complete. Empty result at tablet " << Self->TabletID());
        ctx.Send(Ev->Get()->GetSource(), Result.release());
    } else {
        LOG_S_DEBUG("TTxRead.Complete at tablet " << Self->TabletID() << " Metadata: " << *ReadMetadata);

        const ui64 requestCookie = Self->InFlightReadsTracker.AddInFlightRequest(
            std::static_pointer_cast<const NOlap::TReadMetadataBase>(ReadMetadata), *Self->BlobManager);
        auto statsDelta = Self->InFlightReadsTracker.GetSelectStatsDelta();

        Self->IncCounter(COUNTER_READ_INDEX_GRANULES, statsDelta.Granules);
        Self->IncCounter(COUNTER_READ_INDEX_PORTIONS, statsDelta.Portions);
        Self->IncCounter(COUNTER_READ_INDEX_BLOBS, statsDelta.Blobs);
        Self->IncCounter(COUNTER_READ_INDEX_ROWS, statsDelta.Rows);
        Self->IncCounter(COUNTER_READ_INDEX_BYTES, statsDelta.Bytes);

        TInstant deadline = TInstant::Max(); // TODO
        ctx.Register(CreateReadActor(Self->TabletID(), Ev->Get()->GetSource(),
            std::move(Result), ReadMetadata, deadline, Self->SelfId(), requestCookie));
    }
}


void TColumnShard::Handle(TEvColumnShard::TEvRead::TPtr& ev, const TActorContext& ctx) {
    const auto* msg = ev->Get();
    TRowVersion readVersion(msg->Record.GetPlanStep(), msg->Record.GetTxId());
    TRowVersion maxReadVersion = GetMaxReadVersion();
    LOG_S_DEBUG("Read at tablet " << TabletID() << " version=" << readVersion << " readable=" << maxReadVersion);

    if (maxReadVersion < readVersion) {
        WaitingReads.emplace(readVersion, std::move(ev));
        WaitPlanStep(readVersion.Step);
        return;
    }

    Execute(new TTxRead(this, ev), ctx);
}

}
