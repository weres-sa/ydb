#pragma once

#include "defs.h"
#include "blobstorage_hullreplwritesst.h"
#include "blobstorage_repl.h"

namespace NKikimr {

    namespace NRepl {

        enum class ETimeState : ui32 {
            PREPARE_PLAN,
            TOKEN_WAIT,
            PROXY_WAIT,
            MERGE,
            PDISK_OP,
            COMMIT,
            OTHER,
            PHANTOM,
            COUNT
        };

        struct TTimeAccount {
            TTimeAccount()
                : CurrentState(ETimeState::COUNT)
            {}

            void SetState(ETimeState state) {
                if (CurrentState != state) {
                    TInstant timestamp = TAppData::TimeProvider->Now();
                    if (CurrentState != ETimeState::COUNT)
                        Durations[static_cast<ui32>(CurrentState)] += timestamp - PrevTimestamp;
                    CurrentState = state;
                    PrevTimestamp = timestamp;
                }
            }

            void UpdateInfo(TEvReplFinished::TInfo& replInfo) const {
                replInfo.PreparePlanDuration = Durations[static_cast<ui32>(ETimeState::PREPARE_PLAN)];
                replInfo.TokenWaitDuration = Durations[static_cast<ui32>(ETimeState::TOKEN_WAIT)];
                replInfo.ProxyWaitDuration = Durations[static_cast<ui32>(ETimeState::PROXY_WAIT)];
                replInfo.MergeDuration = Durations[static_cast<ui32>(ETimeState::MERGE)];
                replInfo.PDiskDuration = Durations[static_cast<ui32>(ETimeState::PDISK_OP)];
                replInfo.CommitDuration = Durations[static_cast<ui32>(ETimeState::COMMIT)];
                replInfo.OtherDuration = Durations[static_cast<ui32>(ETimeState::OTHER)];
                replInfo.PhantomDuration = Durations[static_cast<ui32>(ETimeState::PHANTOM)];
            }

        private:
            ETimeState CurrentState;
            TInstant PrevTimestamp;
            TDuration Durations[static_cast<ui32>(ETimeState::COUNT)];
        };

        ////////////////////////////////////////////////////////////////////////////
        // TRecoveryMachine
        ////////////////////////////////////////////////////////////////////////////
        class TRecoveryMachine {
        public:
            using TRecoveredBlobInfo = TReplSstStreamWriter::TRecoveredBlobInfo;
            using TRecoveredBlobsQueue = TQueue<TRecoveredBlobInfo>;

            struct TPartSet {
                TDataPartSet PartSet;
                ui32 DisksRepliedOK = 0;
                ui32 DisksRepliedNODATA = 0;
                ui32 DisksRepliedNOT_YET = 0;
                ui32 DisksRepliedOther = 0;

                TPartSet(TBlobStorageGroupType gtype) {
                    PartSet.Parts.resize(gtype.TotalPartCount());
                }

                void AddData(ui32 diskIdx, const TLogoBlobID& id, NKikimrProto::EReplyStatus status, TString data) {
                    switch (status) {
                        case NKikimrProto::OK: {
                            const ui8 partIdx = id.PartId() - 1;
                            Y_VERIFY(partIdx < PartSet.Parts.size());
                            PartSet.FullDataSize = id.BlobSize();
                            PartSet.PartsMask |= 1 << partIdx;
                            PartSet.Parts[partIdx].ReferenceTo(data);
                            DisksRepliedOK |= 1 << diskIdx;
                            break;
                        }

                        case NKikimrProto::NODATA:
                            DisksRepliedNODATA |= 1 << diskIdx;
                            break;

                        case NKikimrProto::NOT_YET:
                            DisksRepliedNOT_YET |= 1 << diskIdx;
                            break;

                        default:
                            DisksRepliedOther |= 1 << diskIdx;
                            break;
                    }
                }

                TString ToString() const {
                    return TStringBuilder() << "{DisksRepliedOK# " << DisksRepliedOK
                        << " DisksRepliedNODATA# " << DisksRepliedNODATA
                        << " DisksRepliedNOT_YET# " << DisksRepliedNOT_YET
                        << " DisksRepliedOther# " << DisksRepliedOther
                        << "}";
                }
            };

        public:
            TRecoveryMachine(
                    std::shared_ptr<TReplCtx> replCtx,
                    TEvReplFinished::TInfoPtr replInfo,
                    TBlobIdQueuePtr unreplicatedBlobsPtr)
                : ReplCtx(std::move(replCtx))
                , ReplInfo(replInfo)
                , UnreplicatedBlobsPtr(std::move(unreplicatedBlobsPtr))
                , LostVec(TMemoryConsumer(ReplCtx->VCtx->Replication))
                , Arena(&TRopeArenaBackend::Allocate)
            {}

            bool Recover(const TLogoBlobID& id, TPartSet& partSet, TRecoveredBlobsQueue& rbq, NMatrix::TVectorType& parts) {
                Y_VERIFY(!id.PartId());
                Y_VERIFY(!LastRecoveredId || *LastRecoveredId < id);
                LastRecoveredId = id;

                RecoverMetadata(id, rbq);

                while (!LostVec.empty() && LostVec.front().Id < id) {
                    SkipItem(LostVec.front());
                    LostVec.pop_front();
                }

                if (LostVec.empty() || LostVec.front().Id != id) {
                    STLOG(PRI_ERROR, BS_REPL, BSVR27, VDISKP(ReplCtx->VCtx->VDiskLogPrefix, "blob not in LostVec"),
                        (BlobId, id));
                    return true;
                }

                const TLost& lost = LostVec.front();
                Y_VERIFY(lost.Id == id);

                const TBlobStorageGroupType groupType = ReplCtx->VCtx->Top->GType;

                parts = lost.PartsToRecover;

                ui32 partsSize = 0;
                bool hasExactParts = false;
                bool needToRestore = false;
                for (ui8 i = parts.FirstPosition(); i != parts.GetSize(); i = parts.NextPosition(i)) {
                    if (partSet.PartSet.PartsMask & (1 << i)) {
                        hasExactParts = true;
                    } else {
                        needToRestore = true;
                    }
                }

                Y_VERIFY_DEBUG((partSet.PartSet.PartsMask >> groupType.TotalPartCount()) == 0);
                const ui32 presentParts = PopCount(partSet.PartSet.PartsMask);
                bool canRestore = presentParts >= groupType.MinimalRestorablePartCount();

                if (lost.PossiblePhantom && needToRestore && !canRestore) {
                    ReplInfo->DataRecoveryNoParts++;
                    ReplInfo->PartsMissing += parts.CountBits();
                    ++ReplInfo->DataRecoveryPhantomCheck;
                    UnreplicatedBlobsPtr->push_back(id); // treat this blob as non-phantom by default, sort it out later
                    LostVec.pop_front();
                    return false;
                }

                // first of all, count present parts and recover only if there are enough of these parts
                if (!canRestore && needToRestore && !hasExactParts) {
                    ReplInfo->DataRecoveryNoParts++;
                    ReplInfo->PartsMissing += parts.CountBits();
                    STLOG(PRI_INFO, BS_REPL, BSVR28, VDISKP(ReplCtx->VCtx->VDiskLogPrefix, "not enough data parts to recover"),
                        (BlobId, id), (NumPresentParts, presentParts), (MinParts, groupType.DataParts()),
                        (PartSet, partSet.ToString()), (Ingress, lost.Ingress.ToString(ReplCtx->VCtx->Top.get(),
                        ReplCtx->VCtx->ShortSelfVDisk, id)));
                    UnreplicatedBlobsPtr->push_back(id);
                } else {
                    // recover
                    try {
                        Y_VERIFY(partSet.PartSet.FullDataSize == id.BlobSize());

                        // PartSet contains some data, other data will be restored and written in the same PartSet
                        TRope recoveredData;
                        const ui32 incomingMask = partSet.PartSet.PartsMask;
                        if (canRestore && needToRestore) {
                            groupType.RestoreData((TErasureType::ECrcMode)id.CrcMode(), partSet.PartSet, recoveredData,
                                true, false, true);
                            partSet.PartSet.PartsMask = (1 << groupType.TotalPartCount()) - 1;
                        }

                        ui32 numSmallParts = 0, numMissingParts = 0, numHuge = 0;
                        std::array<TRope, 8> partData; // part data for small blobs
                        NMatrix::TVectorType small(0, parts.GetSize());

                        for (ui8 i = parts.FirstPosition(); i != parts.GetSize(); i = parts.NextPosition(i)) {
                            if (~partSet.PartSet.PartsMask & (1 << i)) {
                                ++numMissingParts; // ignore this missing part
                                continue;
                            }
                            if (incomingMask & (1 << i)) {
                                ++ReplInfo->PartsExact;
                            } else {
                                ++ReplInfo->PartsRestored;
                            }
                            const TLogoBlobID partId(id, i + 1);
                            const ui32 partSize = groupType.PartSize(partId);
                            Y_VERIFY(partSize); // no metadata here
                            partsSize += partSize;
                            TRope data(partSet.PartSet.Parts[i].OwnedString); // TODO(alexvru): employ rope in TDataPartSet
                            Y_VERIFY(data.GetSize() == partSize);
                            if (ReplCtx->HugeBlobCtx->IsHugeBlob(groupType, id)) {
                                AddBlobToQueue(partId, TDiskBlob::Create(partSet.PartSet.FullDataSize, i + 1,
                                    groupType.TotalPartCount(), std::move(data), Arena), {}, true, rbq);
                                ++numHuge;
                            } else {
                                partData[numSmallParts++] = std::move(data);
                                small.Set(i);
                            }
                        }

                        if (numMissingParts) {
                            // this blob is not fully replicated yet
                            UnreplicatedBlobsPtr->push_back(id);
                        }

                        if (numSmallParts) {
                            // fill in disk blob buffer
                            AddBlobToQueue(id, TDiskBlob::CreateFromDistinctParts(&partData[0], &partData[numSmallParts],
                                small, partSet.PartSet.FullDataSize, Arena), small, false, rbq);
                        }

                        ReplInfo->LogoBlobsRecovered += numSmallParts;
                        ReplInfo->HugeLogoBlobsRecovered += numHuge;
                        ReplInfo->BytesRecovered += partsSize;
                        ReplInfo->PartsMissing += numMissingParts;
                        ReplInfo->DataRecoverySuccess++;
                        ReplInfo->WorkUnitsProcessed += id.BlobSize();
                        Y_VERIFY_DEBUG(ReplInfo->WorkUnitsProcessed <= ReplInfo->WorkUnitsPlanned);

                        if (!numMissingParts) {
                            ReplCtx->MonGroup.ReplWorkUnitsDone() += id.BlobSize();
                            ReplCtx->MonGroup.ReplWorkUnitsRemaining() -= id.BlobSize();
                        }
                        if (lost.PossiblePhantom) {
                            ++ReplCtx->MonGroup.ReplPhantomLikeRecovered();
                        }
                    } catch (const std::exception& ex) {
                        ++ReplCtx->MonGroup.ReplRecoveryGroupTypeErrors();
                        STLOG(PRI_ERROR, BS_REPL, BSVR29, VDISKP(ReplCtx->VCtx->VDiskLogPrefix, "recovery exception"),
                            (BlobId, id), (Error, TString(ex.what())));
                        ReplInfo->DataRecoveryFailure++;
                        UnreplicatedBlobsPtr->push_back(id);
                    }
                }

                LostVec.pop_front();
                return true;
            }

            void ProcessPhantomBlob(const TLogoBlobID& id, NMatrix::TVectorType parts, bool isPhantom) {
                STLOG(PRI_INFO, BS_REPL, BSVR00, VDISKP(ReplCtx->VCtx->VDiskLogPrefix, "phantom check completed"),
                    (BlobId, id), (Parts, parts), (IsPhantom, isPhantom));

                ++(isPhantom
                    ? ReplCtx->MonGroup.ReplPhantomLikeDropped()
                    : ReplCtx->MonGroup.ReplPhantomLikeUnrecovered());
            }

            // finish work
            void Finish(TRecoveredBlobsQueue& rbq) {
                RecoverMetadata(TLogoBlobID(Max<ui64>(), Max<ui64>(), Max<ui64>()), rbq);
                for (auto&& item : LostVec) {
                    SkipItem(item);
                }
                LostVec.clear();
            }

            // add next task during preparation phase
            void AddTask(const TLogoBlobID &id, const NMatrix::TVectorType &partsToRecover, bool possiblePhantom,
                    TIngress ingress) {
                Y_VERIFY(!id.PartId());
                Y_VERIFY(LostVec.empty() || LostVec.back().Id < id);
                LostVec.push_back(TLost(id, partsToRecover, possiblePhantom, ingress));
            }

            void AddMetadataPart(const TLogoBlobID& id) {
                MetadataParts.push_back(id);
            }

            bool FullOfTasks() const {
                return LostVec.size() >= ReplCtx->VDiskCfg->ReplMaxLostVecSize;
            }

            bool NoTasks() const {
                return LostVec.empty() && MetadataParts.empty();
            }

            void ClearPossiblePhantom() {
                for (TLost& item : LostVec) {
                    item.PossiblePhantom = false;
                }
            }

            template<typename TCallback>
            void ForEach(TCallback&& callback) {
                for (const TLost& item : LostVec) {
                    callback(item.Id, item.PartsToRecover, item.Ingress);
                }
            }

        private:
            // structure for a lost part
            struct TLost {
                const TLogoBlobID Id;
                const NMatrix::TVectorType PartsToRecover;
                bool PossiblePhantom;
                const TIngress Ingress;

                TLost(const TLogoBlobID &id, const NMatrix::TVectorType &partsToRecover, const bool possiblePhantom,
                        TIngress ingress)
                    : Id(id)
                    , PartsToRecover(partsToRecover)
                    , PossiblePhantom(possiblePhantom)
                    , Ingress(ingress)
                {}
            };

            // vector of lost parts, we are goind to recover them during this job
            typedef TTrackableDeque<TLost> TLostVec;

            std::shared_ptr<TReplCtx> ReplCtx;
            TEvReplFinished::TInfoPtr ReplInfo;
            TBlobIdQueuePtr UnreplicatedBlobsPtr;
            TLostVec LostVec;
            TDeque<TLogoBlobID> MetadataParts;
            TRopeArena Arena;
            std::optional<TLogoBlobID> LastRecoveredId;

            void AddBlobToQueue(const TLogoBlobID& id, TRope blob, NMatrix::TVectorType parts, bool isHugeBlob,
                    TRecoveredBlobsQueue& rbq) {
                if (!rbq.empty() && rbq.back().Id == id && !isHugeBlob) {
                    auto& last = rbq.back();
                    TDiskBlobMerger merger;
                    merger.Add(TDiskBlob(&last.Data, last.LocalParts, ReplCtx->VCtx->Top->GType, id));
                    merger.Add(TDiskBlob(&blob, parts, ReplCtx->VCtx->Top->GType, id));
                    last.LocalParts = merger.GetDiskBlob().GetParts();
                    last.Data = merger.CreateDiskBlob(Arena);
                } else {
                    rbq.emplace(id, std::move(blob), isHugeBlob, parts);
                }
            }

            void RecoverMetadata(const TLogoBlobID& id, TRecoveredBlobsQueue& rbq) {
                while (!MetadataParts.empty() && MetadataParts.front().FullID() <= id) {
                    const TLogoBlobID id = MetadataParts.front();
                    const bool isHugeBlob = ReplCtx->HugeBlobCtx->IsHugeBlob(ReplCtx->VCtx->Top->GType, id.FullID());
                    MetadataParts.pop_front();
                    STLOG(PRI_DEBUG, BS_REPL, BSVR30, VDISKP(ReplCtx->VCtx->VDiskLogPrefix,
                        "TRecoveryMachine::RecoverMetadata"), (BlobId, id));
                    const TBlobStorageGroupType gtype = ReplCtx->VCtx->Top->GType;
                    if (isHugeBlob) {
                        // huge metadata blob contains ID with designated part id and no data at all (and no parts vector)
                        AddBlobToQueue(id, TRope(), {}, true, rbq);
                    } else {
                        // small metadata blob contains only header without data, but its ID has PartId = 0 and parts
                        // vector is filled accordingly
                        const NMatrix::TVectorType parts = NMatrix::TVectorType::MakeOneHot(id.PartId() - 1,
                            gtype.TotalPartCount());
                        AddBlobToQueue(id.FullID(), TDiskBlob::Create(id.BlobSize(), parts, TRope(),
                            Arena), parts, isHugeBlob, rbq);
                    }
                    ++ReplInfo->MetadataBlobs;
                }
            }

            void SkipItem(const TLost& item) {
                STLOG(PRI_INFO, BS_REPL, BSVR31, VDISKP(ReplCtx->VCtx->VDiskLogPrefix, "TRecoveryMachine::SkipItem"),
                    (BlobId, item.Id));
                ++ReplInfo->DataRecoverySkip;
                UnreplicatedBlobsPtr->push_back(item.Id);
                if (item.PossiblePhantom) {
                    ++ReplCtx->MonGroup.ReplPhantomLikeUnrecovered();
                }
            }
        };

    } // NRepl

} // NKikimr
