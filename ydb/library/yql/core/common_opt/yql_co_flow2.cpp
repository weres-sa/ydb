#include "yql_co_extr_members.h"
#include "yql_co.h"

#include <ydb/library/yql/core/yql_expr_type_annotation.h>
#include <ydb/library/yql/core/yql_join.h>
#include <ydb/library/yql/core/yql_expr_optimize.h>
#include <ydb/library/yql/core/yql_opt_utils.h>
#include <ydb/library/yql/core/yql_opt_window.h>
#include <ydb/library/yql/core/yql_type_helpers.h>

#include <ydb/library/yql/utils/log/log.h>

namespace NYql {
namespace {

using namespace NNodes;

TExprNode::TPtr AggregateSubsetFieldsAnalyzer(const TCoAggregate& node, TExprContext& ctx, const TParentsMap& parentsMap) {
    auto inputType = node.Input().Ref().GetTypeAnn();
    auto structType = inputType->GetKind() == ETypeAnnotationKind::List
        ? inputType->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>()
        : inputType->Cast<TStreamExprType>()->GetItemType()->Cast<TStructExprType>();

    if (structType->GetSize() == 0) {
        return node.Ptr();
    }

    TMaybe<TStringBuf> sessionColumn;
    const auto sessionSetting = GetSetting(node.Settings().Ref(), "session");
    if (sessionSetting) {
        YQL_ENSURE(sessionSetting->Child(1)->Child(0)->IsAtom());
        sessionColumn = sessionSetting->Child(1)->Child(0)->Content();
    }

    TSet<TStringBuf> usedFields;
    for (const auto& x : node.Keys()) {
        if (x.Value() != sessionColumn) {
            usedFields.insert(x.Value());
        }
    }

    if (usedFields.size() == structType->GetSize()) {
        return node.Ptr();
    }

    for (const auto& x : node.Handlers()) {
        if (x.Ref().ChildrenSize() == 3) {
            // distinct field
            usedFields.insert(x.Ref().Child(2)->Content());
        }
        else {
            auto traits = x.Ref().Child(1);
            ui32 index;
            if (traits->IsCallable("AggregationTraits")) {
                index = 0;
            } else if (traits->IsCallable("AggApply")) {
                index = 1;
            } else {
                return node.Ptr();
            }

            auto structType = traits->Child(index)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
            for (const auto& item : structType->GetItems()) {
                usedFields.insert(item->GetName());
            }
        }

        if (usedFields.size() == structType->GetSize()) {
            return node.Ptr();
        }
    }

    auto settings = node.Settings();
    auto hoppingSetting = GetSetting(settings.Ref(), "hopping");
    if (hoppingSetting) {
        auto traits = TCoHoppingTraits(hoppingSetting->Child(1));
        auto timeExtractor = traits.TimeExtractor();

        auto usedType = traits.ItemType().Ref().GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
        for (const auto& usedField : usedType->GetItems()) {
            usedFields.insert(usedField->GetName());
        }

        TSet<TStringBuf> lambdaSubset;
        if (!HaveFieldsSubset(timeExtractor.Body().Ptr(), *timeExtractor.Args().Arg(0).Raw(), lambdaSubset, parentsMap)) {
            return node.Ptr();
        }
        usedFields.insert(lambdaSubset.cbegin(), lambdaSubset.cend());

        if (usedFields.size() == structType->GetSize()) {
            return node.Ptr();
        }
    }

    if (sessionSetting) {
        TCoSessionWindowTraits traits(sessionSetting->Child(1)->ChildPtr(1));

        auto usedType = traits.ListType().Ref().GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TListExprType>()->
            GetItemType()->Cast<TStructExprType>();
        for (const auto& item : usedType->GetItems()) {
            usedFields.insert(item->GetName());
        }

        if (usedFields.size() == structType->GetSize()) {
            return node.Ptr();
        }
    }

    TExprNode::TListType keepMembersList;
    for (const auto& x : usedFields) {
        keepMembersList.push_back(ctx.NewAtom(node.Pos(), x));
    }

    auto newInput = ctx.Builder(node.Pos())
        .Callable("ExtractMembers")
            .Add(0, node.Input().Ptr())
            .Add(1, ctx.NewList(node.Pos(), std::move(keepMembersList)))
        .Seal()
        .Build();

    auto ret = ctx.ChangeChild(node.Ref(), 0, std::move(newInput));
    return ret;
}

TExprNode::TPtr ConstantPredicatePushdownOverEquiJoin(TExprNode::TPtr equiJoin, TExprNode::TPtr predicate, bool ordered, TExprContext& ctx) {
    auto lambda = ctx.Builder(predicate->Pos())
        .Lambda()
            .Param("row")
            .Set(predicate)
        .Seal()
        .Build();

    auto ret = ctx.ShallowCopy(*equiJoin);
    auto inputsCount = ret->ChildrenSize() - 2;
    for (ui32 i = 0; i < inputsCount; ++i) {
        ret->ChildRef(i) = ctx.ShallowCopy(*ret->Child(i));
        ret->Child(i)->ChildRef(0) = ctx.Builder(predicate->Pos())
            .Callable(ordered ? "OrderedFilter" : "Filter")
                .Add(0, ret->Child(i)->ChildPtr(0))
                .Add(1, lambda)
            .Seal()
            .Build();
    }

    return ret;
}

void GatherKeyAliases(const TExprNode::TPtr& joinTree, TMap<TString, TSet<TString>>& aliases, const TJoinLabels& labels) {
    auto left = joinTree->ChildPtr(1);
    if (!left->IsAtom()) {
        GatherKeyAliases(left, aliases, labels);
    }

    auto right = joinTree->ChildPtr(2);
    if (!right->IsAtom()) {
        GatherKeyAliases(right, aliases, labels);
    }

    auto leftColumns = joinTree->Child(3);
    auto rightColumns = joinTree->Child(4);
    for (ui32 i = 0; i < leftColumns->ChildrenSize(); i += 2) {
        auto leftColumn = FullColumnName(leftColumns->Child(i)->Content(), leftColumns->Child(i + 1)->Content());
        auto rightColumn = FullColumnName(rightColumns->Child(i)->Content(), rightColumns->Child(i + 1)->Content());
        auto leftType = *labels.FindColumn(leftColumn);
        auto rightType = *labels.FindColumn(rightColumn);
        if (IsSameAnnotation(*leftType, *rightType)) {
            aliases[leftColumn].insert(rightColumn);
            aliases[rightColumn].insert(leftColumn);
        }
    }
}

void MakeTransitiveClosure(TMap<TString, TSet<TString>>& aliases) {
    for (;;) {
        bool hasChanges = false;
        for (auto& x : aliases) {
            for (auto& y : x.second) {
                // x.first->y
                for (auto& z : aliases[y]) {
                    // add x.first->z
                    if (x.first != z) {
                        hasChanges = x.second.insert(z).second || hasChanges;
                    }
                }
            }
        }

        if (!hasChanges) {
            return;
        }
    }
}

void GatherOptionalKeyColumnsFromEquality(TExprNode::TPtr columns, const TJoinLabels& labels, ui32 inputIndex,
    TSet<TString>& optionalKeyColumns) {
    for (ui32 i = 0; i < columns->ChildrenSize(); i += 2) {
        auto table = columns->Child(i)->Content();
        auto column = columns->Child(i + 1)->Content();
        if (*labels.FindInputIndex(table) == inputIndex) {
            auto type = *labels.FindColumn(table, column);
            if (type->GetKind() == ETypeAnnotationKind::Optional) {
                optionalKeyColumns.insert(FullColumnName(table, column));
            }
        }
    }
}

void GatherOptionalKeyColumns(TExprNode::TPtr joinTree, const TJoinLabels& labels, ui32 inputIndex,
    TSet<TString>& optionalKeyColumns) {
    auto left = joinTree->Child(1);
    auto right = joinTree->Child(2);
    if (!left->IsAtom()) {
        GatherOptionalKeyColumns(left, labels, inputIndex, optionalKeyColumns);
    }

    if (!right->IsAtom()) {
        GatherOptionalKeyColumns(right, labels, inputIndex, optionalKeyColumns);
    }

    auto joinType = joinTree->Child(0)->Content();
    if (joinType == "Inner" || joinType == "LeftSemi") {
        GatherOptionalKeyColumnsFromEquality(joinTree->Child(3), labels, inputIndex, optionalKeyColumns);
    }

    if (joinType == "Inner" || joinType == "RightSemi") {
        GatherOptionalKeyColumnsFromEquality(joinTree->Child(4), labels, inputIndex, optionalKeyColumns);
    }
}

TExprNode::TPtr SingleInputPredicatePushdownOverEquiJoin(TExprNode::TPtr equiJoin, TExprNode::TPtr predicate,
    const TSet<TStringBuf>& usedFields, TExprNode::TPtr args, const TJoinLabels& labels,
    ui32 firstCandidate, const TMap<TStringBuf, TVector<TStringBuf>>& renameMap, bool ordered, TExprContext& ctx) {
    auto inputsCount = equiJoin->ChildrenSize() - 2;
    auto joinTree = equiJoin->Child(inputsCount);
    TMap<TString, TSet<TString>> aliases;
    GatherKeyAliases(joinTree, aliases, labels);
    MakeTransitiveClosure(aliases);
    TSet<ui32> candidates;
    candidates.insert(firstCandidate);
    // check whether some used fields are not aliased
    bool onlyKeys = true;
    for (auto& x : usedFields) {
        if (!aliases.contains(TString(x))) {
            onlyKeys = false;
            break;
        }
    }

    THashMap<ui32, THashMap<TString, TString>> aliasedKeys;
    if (onlyKeys) {
        // try to extend inputs
        for (ui32 i = 0; i < inputsCount; ++i) {
            if (i == firstCandidate) {
                continue;
            }

            TSet<TString> coveredKeys;
            for (auto field : labels.Inputs[i].EnumerateAllColumns()) {
                if (auto aliasSet = aliases.FindPtr(field)) {
                    for (auto alias : *aliasSet) {
                        if (usedFields.contains(alias)) {
                            coveredKeys.insert(TString(alias));
                            aliasedKeys[i].insert({ field, TString(alias) });
                        }
                    }
                }
            }

            if (coveredKeys.size() == usedFields.size()) {
                candidates.insert(i);
            }
        }
    }

    if (!IsRequiredSide(joinTree, labels, firstCandidate).first) {
        return equiJoin;
    }

    auto ret = ctx.ShallowCopy(*equiJoin);
    for (auto& inputIndex : candidates) {
        auto x = IsRequiredSide(joinTree, labels, inputIndex);
        if (!x.first) {
            continue;
        }

        auto prevInput = equiJoin->Child(inputIndex)->ChildPtr(0);
        auto newInput = prevInput;
        if (x.second) {
            // skip null key columns
            TSet<TString> optionalKeyColumns;
            GatherOptionalKeyColumns(joinTree, labels, inputIndex, optionalKeyColumns);
            newInput = FilterOutNullJoinColumns(predicate->Pos(),
                prevInput, labels.Inputs[inputIndex], optionalKeyColumns, ctx);
        }

        // then apply predicate
        newInput = ctx.Builder(predicate->Pos())
            .Callable(ordered ? "OrderedFilter" : "Filter")
                .Add(0, newInput)
                .Lambda(1)
                    .Param("row")
                    .ApplyPartial(args, predicate).With(0)
                        .Callable("AsStruct")
                            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                                ui32 index = 0;
                                const auto& label = labels.Inputs[inputIndex];
                                for (auto column : label.EnumerateAllColumns()) {
                                    TVector<TString> targetColumns;
                                    targetColumns.push_back(column);
                                    if (onlyKeys && inputIndex != firstCandidate) {
                                        if (auto aliasedKey = aliasedKeys[inputIndex].FindPtr(column)) {
                                            targetColumns[0] = *aliasedKey;
                                        } else {
                                            continue;
                                        }
                                    }

                                    TStringBuf part1;
                                    TStringBuf part2;
                                    SplitTableName(column, part1, part2);
                                    auto memberName = label.MemberName(part1, part2);

                                    if (auto renamed = renameMap.FindPtr(targetColumns[0])) {
                                        if (renamed->empty()) {
                                            continue;
                                        }

                                        targetColumns.clear();
                                        for (auto& r : *renamed) {
                                            targetColumns.push_back(TString(r));
                                        }
                                    }

                                    for (auto targetColumn : targetColumns) {
                                        parent.List(index++)
                                                .Atom(0, targetColumn)
                                                .Callable(1, "Member")
                                                    .Arg(0, "row")
                                                    .Atom(1, memberName)
                                                .Seal()
                                            .Seal();
                                    }
                                }

                                return parent;
                            })
                        .Seal()
                    .Done().Seal()
                .Seal()
            .Seal()
            .Build();

        // then return reassembled join
        ret->ChildRef(inputIndex) = ctx.ShallowCopy(*ret->Child(inputIndex));
        ret->Child(inputIndex)->ChildRef(0) = newInput;
    }

    return ret;
}

void GatherJoinInputs(const TExprNode::TPtr& expr, const TExprNode& row,
    const TParentsMap& parentsMap, const THashMap<TString, TString>& backRenameMap,
    const TJoinLabels& labels, TSet<ui32>& inputs, TSet<TStringBuf>& usedFields) {
    usedFields.clear();

    if (!HaveFieldsSubset(expr, row, usedFields, parentsMap, false)) {
        const auto inputStructType = RemoveOptionalType(row.GetTypeAnn())->Cast<TStructExprType>();
        for (const auto& i : inputStructType->GetItems()) {
            usedFields.insert(i->GetName());
        }
    }

    for (auto x : usedFields) {
        // rename used fields
        if (auto renamed = backRenameMap.FindPtr(x)) {
            x = *renamed;
        }

        TStringBuf part1;
        TStringBuf part2;
        SplitTableName(x, part1, part2);
        inputs.insert(*labels.FindInputIndex(part1));
        if (inputs.size() == labels.Inputs.size()) {
            break;
        }
    }
}

class TJoinTreeRebuilder {
public:
    TJoinTreeRebuilder(TExprNode::TPtr joinTree, TStringBuf label1, TStringBuf column1, TStringBuf label2, TStringBuf column2,
        TExprContext& ctx)
        : JoinTree(joinTree)
        , Labels{ label1, label2 }
        , Columns{ column1, column2 }
        , Ctx(ctx)
    {}

    TExprNode::TPtr Run() {
        auto joinTree = RotateCrossJoin(JoinTree->Pos(), JoinTree);
        auto newJoinTree = std::get<0>(AddLink(joinTree));
        YQL_ENSURE(Updated);
        return newJoinTree;
    }

private:
    TExprNode::TPtr RotateCrossJoin(TPositionHandle pos, TExprNode::TPtr joinTree) {
        if (joinTree->Child(0)->Content() != "Cross") {
            auto children = joinTree->ChildrenList();
            auto& left = children[1];
            auto& right = children[2];

            if (!left->IsAtom()) {
                left = RotateCrossJoin(pos, left);
            }

            if (!right->IsAtom()) {
                right = RotateCrossJoin(pos, right);
            }

            return Ctx.ChangeChildren(*joinTree, std::move(children));
        }

        CrossJoins.clear();
        RestJoins.clear();
        GatherCross(joinTree);
        auto inCross1 = FindPtr(CrossJoins, Labels[0]);
        auto inCross2 = FindPtr(CrossJoins, Labels[1]);
        if (inCross1 || inCross2) {
            if (inCross1 && inCross2) {
                // make them a leaf
                joinTree = MakeCrossJoin(pos, Ctx.NewAtom(pos, Labels[0]), Ctx.NewAtom(pos, Labels[1]), Ctx);
                for (auto label : CrossJoins) {
                    if (label != Labels[0] && label != Labels[1]) {
                        joinTree = MakeCrossJoin(pos, joinTree, Ctx.NewAtom(pos, label), Ctx);
                    }
                }

                joinTree = AddRestJoins(pos, joinTree, nullptr);
            } else if (inCross1) {
                // leaf with table1 and subtree with table2
                auto rest = FindRestJoin(Labels[1]);
                YQL_ENSURE(rest);
                joinTree = MakeCrossJoin(pos, Ctx.NewAtom(pos, Labels[0]), rest, Ctx);
                for (auto label : CrossJoins) {
                    if (label != Labels[0]) {
                        joinTree = MakeCrossJoin(pos, joinTree, Ctx.NewAtom(pos, label), Ctx);
                    }
                }

                joinTree = AddRestJoins(pos, joinTree, rest);
            } else {
                // leaf with table2 and subtree with table1
                auto rest = FindRestJoin(Labels[0]);
                YQL_ENSURE(rest);
                joinTree = MakeCrossJoin(pos, Ctx.NewAtom(pos, Labels[1]), rest, Ctx);
                for (auto label : CrossJoins) {
                    if (label != Labels[1]) {
                        joinTree = MakeCrossJoin(pos, joinTree, Ctx.NewAtom(pos, label), Ctx);
                    }
                }

                joinTree = AddRestJoins(pos, joinTree, rest);
            }
        }

        return joinTree;
    }

    TExprNode::TPtr AddRestJoins(TPositionHandle pos, TExprNode::TPtr joinTree, TExprNode::TPtr exclude) {
        for (auto join : RestJoins) {
            if (join == exclude) {
                continue;
            }

            joinTree = MakeCrossJoin(pos, joinTree, join, Ctx);
        }

        return joinTree;
    }

    TExprNode::TPtr FindRestJoin(TStringBuf label) {
        for (auto join : RestJoins) {
            if (HasTable(join, label)) {
                return join;
            }
        }

        return nullptr;
    }

    bool HasTable(TExprNode::TPtr joinTree, TStringBuf label) {
        auto left = joinTree->ChildPtr(1);
        if (left->IsAtom()) {
            if (left->Content() == label) {
                return true;
            }
        } else {
            if (HasTable(left, label)) {
                return true;
            }
        }

        auto right = joinTree->ChildPtr(2);
        if (right->IsAtom()) {
            if (right->Content() == label) {
                return true;
            }
        } else {
            if (HasTable(right, label)) {
                return true;
            }
        }

        return false;
    }

    void GatherCross(TExprNode::TPtr joinTree) {
        auto type = joinTree->Child(0)->Content();
        if (type != "Cross") {
            RestJoins.push_back(joinTree);
            return;
        }

        auto left = joinTree->ChildPtr(1);
        if (left->IsAtom()) {
            CrossJoins.push_back(left->Content());
        } else {
            GatherCross(left);
        }

        auto right = joinTree->ChildPtr(2);
        if (right->IsAtom()) {
            CrossJoins.push_back(right->Content());
        } else {
            GatherCross(right);
        }
    }

    std::tuple<TExprNode::TPtr, TMaybe<ui32>, TMaybe<ui32>> AddLink(TExprNode::TPtr joinTree) {
        auto children = joinTree->ChildrenList();

        TMaybe<ui32> found1;
        TMaybe<ui32> found2;
        auto& left = children[1];
        if (!left->IsAtom()) {
            TMaybe<ui32> leftFound1, leftFound2;
            std::tie(left, leftFound1, leftFound2) = AddLink(left);
            if (leftFound1) {
                found1 = 1u;
            }

            if (leftFound2) {
                found2 = 1u;
            }
        } else {
            if (left->Content() == Labels[0]) {
                found1 = 1u;
            }

            if (left->Content() == Labels[1]) {
                found2 = 1u;
            }
        }

        auto& right = children[2];
        if (!right->IsAtom()) {
            TMaybe<ui32> rightFound1, rightFound2;
            std::tie(right, rightFound1, rightFound2) = AddLink(right);
            if (rightFound1) {
                found1 = 2u;
            }

            if (rightFound2) {
                found2 = 2u;
            }
        } else {
            if (right->Content() == Labels[0]) {
                found1 = 2u;
            }

            if (right->Content() == Labels[1]) {
                found2 = 2u;
            }
        }

        if (found1 && found2) {
            if (!Updated) {
                if (joinTree->Child(0)->Content() == "Cross") {
                    children[0] = Ctx.NewAtom(joinTree->Pos(), "Inner");
                } else {
                    YQL_ENSURE(joinTree->Child(0)->Content() == "Inner");
                }

                ui32 index1 = *found1 - 1; // 0/1
                ui32 index2 = 1 - index1;

                auto link1 = children[3]->ChildrenList();
                link1.push_back(Ctx.NewAtom(joinTree->Pos(), Labels[index1]));
                link1.push_back(Ctx.NewAtom(joinTree->Pos(), Columns[index1]));
                children[3] = Ctx.ChangeChildren(*children[3], std::move(link1));

                auto link2 = children[4]->ChildrenList();
                link2.push_back(Ctx.NewAtom(joinTree->Pos(), Labels[index2]));
                link2.push_back(Ctx.NewAtom(joinTree->Pos(), Columns[index2]));
                children[4] = Ctx.ChangeChildren(*children[4], std::move(link2));

                Updated = true;
            }
        }

        return { Ctx.ChangeChildren(*joinTree, std::move(children)), found1, found2 };
    }

private:
    TVector<TStringBuf> CrossJoins;
    TVector<TExprNode::TPtr> RestJoins;

    bool Updated = false;

    TExprNode::TPtr JoinTree;
    TStringBuf Labels[2];
    TStringBuf Columns[2];
    TExprContext& Ctx;
};

TExprNode::TPtr DecayCrossJoinIntoInner(TExprNode::TPtr equiJoin, const TExprNode::TPtr& predicate,
    const TJoinLabels& labels, ui32 index1, ui32 index2,  const TExprNode& row, const THashMap<TString, TString>& backRenameMap,
    const TParentsMap& parentsMap, TExprContext& ctx) {
    YQL_ENSURE(index1 != index2);
    TExprNode::TPtr left, right;
    if (!IsEquality(predicate, left, right)) {
        return equiJoin;
    }

    TSet<ui32> leftInputs, rightInputs;
    TSet<TStringBuf> usedFields;
    GatherJoinInputs(left, row, parentsMap, backRenameMap, labels, leftInputs, usedFields);
    GatherJoinInputs(right, row, parentsMap, backRenameMap, labels, rightInputs, usedFields);
    bool good = false;
    if (leftInputs.size() == 1 && rightInputs.size() == 1) {
        if (*leftInputs.begin() == index1 && *rightInputs.begin() == index2) {
            good = true;
        } else if (*leftInputs.begin() == index2 && *rightInputs.begin() == index1) {
            good = true;
        }
    }

    if (!good) {
        return equiJoin;
    }

    auto inputsCount = equiJoin->ChildrenSize() - 2;
    auto joinTree = equiJoin->Child(inputsCount);
    if (!IsRequiredSide(joinTree, labels, index1).first ||
        !IsRequiredSide(joinTree, labels, index2).first) {
        return equiJoin;
    }

    TStringBuf label1, column1, label2, column2;
    if (left->IsCallable("Member") && left->Child(0) == &row) {
        auto x = left->Tail().Content();
        if (auto ptr = backRenameMap.FindPtr(x)) {
            x = *ptr;
        }

        SplitTableName(x, label1, column1);
    } else {
        return equiJoin;
    }

    if (right->IsCallable("Member") && right->Child(0) == &row) {
        auto x = right->Tail().Content();
        if (auto ptr = backRenameMap.FindPtr(x)) {
            x = *ptr;
        }

        SplitTableName(x, label2, column2);
    } else {
        return equiJoin;
    }

    TJoinTreeRebuilder rebuilder(joinTree, label1, column1, label2, column2, ctx);
    auto newJoinTree = rebuilder.Run();
    return ctx.ChangeChild(*equiJoin, inputsCount, std::move(newJoinTree));
}

TExprNode::TPtr FlatMapOverEquiJoin(const TCoFlatMapBase& node, TExprContext& ctx, const TParentsMap& parentsMap) {
    auto equiJoin = node.Input();
    auto structType = equiJoin.Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType()
        ->Cast<TStructExprType>();
    if (structType->GetSize() == 0) {
        return node.Ptr();
    }

    TExprNode::TPtr structNode;
    if (IsRenameFlatMap(node, structNode)) {
        YQL_CLOG(DEBUG, Core) << "Rename in " << node.CallableName() << " over EquiJoin";
        auto joinSettings = equiJoin.Ref().ChildPtr(equiJoin.Ref().ChildrenSize() - 1);
        auto renameMap = LoadJoinRenameMap(*joinSettings);
        joinSettings = RemoveSetting(*joinSettings, "rename", ctx);
        auto structType = equiJoin.Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType()
            ->Cast<TStructExprType>();
        THashSet<TStringBuf> usedFields;
        TMap<TStringBuf, TVector<TStringBuf>> memberUsageMap;
        for (auto& child : structNode->Children()) {
            auto item = child->Child(1);
            usedFields.insert(item->Child(1)->Content());
            memberUsageMap[item->Child(1)->Content()].push_back(child->Child(0)->Content());
        }

        TMap<TStringBuf, TStringBuf> reversedRenameMap;
        TMap<TStringBuf, TVector<TStringBuf>> newRenameMap;
        for (auto& x : renameMap) {
            if (!x.second.empty()) {
                for (auto& y : x.second) {
                    reversedRenameMap[y] = x.first;
                }
            }
            else {
                // previous drops
                newRenameMap[x.first].clear();
            }
        }

        for (auto& x : structType->GetItems()) {
            if (!usedFields.contains(x->GetName())) {
                // new drops
                auto name = x->GetName();
                if (auto renamed = reversedRenameMap.FindPtr(name)) {
                    name = *renamed;
                }

                newRenameMap[name].clear();
            }
        }

        for (auto& x : memberUsageMap) {
            auto prevName = x.first;
            if (auto renamed = reversedRenameMap.FindPtr(prevName)) {
                prevName = *renamed;
            }

            for (auto& y : x.second) {
                newRenameMap[prevName].push_back(y);
            }
        }

        TExprNode::TListType joinSettingsNodes = joinSettings->ChildrenList();
        AppendEquiJoinRenameMap(node.Pos(), newRenameMap, joinSettingsNodes, ctx);
        joinSettings = ctx.ChangeChildren(*joinSettings, std::move(joinSettingsNodes));
        auto ret = ctx.ShallowCopy(equiJoin.Ref());
        ret->ChildRef(ret->ChildrenSize() - 1) = joinSettings;
        return ret;
    }

    TSet<TStringBuf> usedFields;
    auto& arg = node.Lambda().Args().Arg(0).Ref();
    auto body = node.Lambda().Body().Ptr();
    if (HaveFieldsSubset(body, arg, usedFields, parentsMap)) {
        YQL_CLOG(DEBUG, Core) << "FieldsSubset in " << node.CallableName() << " over EquiJoin";
        auto joinSettings = equiJoin.Ref().ChildPtr(equiJoin.Ref().ChildrenSize() - 1);
        auto renameMap = LoadJoinRenameMap(*joinSettings);
        joinSettings = RemoveSetting(*joinSettings, "rename", ctx);
        auto newRenameMap = UpdateUsedFieldsInRenameMap(renameMap, usedFields, structType);
        auto newLambda = ctx.Builder(node.Pos())
            .Lambda()
            .Param("item")
                .ApplyPartial(node.Lambda().Args().Ptr(), body).With(0, "item").Seal()
            .Seal()
            .Build();

        TExprNode::TListType joinSettingsNodes = joinSettings->ChildrenList();
        AppendEquiJoinRenameMap(node.Pos(), newRenameMap, joinSettingsNodes, ctx);
        joinSettings = ctx.ChangeChildren(*joinSettings, std::move(joinSettingsNodes));
        auto updatedEquiJoin = ctx.ShallowCopy(equiJoin.Ref());
        updatedEquiJoin->ChildRef(updatedEquiJoin->ChildrenSize() - 1) = joinSettings;

        return ctx.Builder(node.Pos())
            .Callable(node.CallableName())
                .Add(0, updatedEquiJoin)
                .Add(1, newLambda)
            .Seal()
            .Build();
    }

    if (IsPredicateFlatMap(node.Lambda().Body().Ref())) {
        // predicate pushdown
        const auto& row = node.Lambda().Args().Arg(0).Ref();
        auto predicate = node.Lambda().Body().Ref().ChildPtr(0);
        auto value = node.Lambda().Body().Ref().ChildPtr(1);
        TJoinLabels labels;
        for (ui32 i = 0; i < equiJoin.Ref().ChildrenSize() - 2; ++i) {
            auto err = labels.Add(ctx, *equiJoin.Ref().Child(i)->Child(1),
                equiJoin.Ref().Child(i)->Child(0)->GetTypeAnn()->Cast<TListExprType>()
                ->GetItemType()->Cast<TStructExprType>());
            if (err) {
                ctx.AddError(*err);
                return nullptr;
            }
        }

        TExprNode::TListType andTerms;
        bool isPg;
        GatherAndTerms(predicate, andTerms, isPg, ctx);
        TExprNode::TPtr ret;
        TExprNode::TPtr extraPredicate;
        auto joinSettings = equiJoin.Ref().Child(equiJoin.Ref().ChildrenSize() - 1);
        auto renameMap = LoadJoinRenameMap(*joinSettings);
        THashMap<TString, TString> backRenameMap;
        for (auto& x : renameMap) {
            if (!x.second.empty()) {
                for (auto& y : x.second) {
                    backRenameMap[y] = x.first;
                }
            }
        }

        const bool ordered = node.Maybe<TCoOrderedFlatMap>().IsValid();

        for (auto& andTerm : andTerms) {
            if (andTerm->IsCallable("Likely")) {
                continue;
            }

            TSet<ui32> inputs;
            GatherJoinInputs(andTerm, row, parentsMap, backRenameMap, labels, inputs, usedFields);

            if (inputs.size() == 0) {
                YQL_CLOG(DEBUG, Core) << "ConstantPredicatePushdownOverEquiJoin";
                ret = ConstantPredicatePushdownOverEquiJoin(equiJoin.Ptr(), andTerm, ordered, ctx);
                extraPredicate = FuseAndTerms(node.Pos(), andTerms, andTerm, isPg, ctx);
                break;
            }

            if (inputs.size() == 1) {
                auto newJoin = SingleInputPredicatePushdownOverEquiJoin(equiJoin.Ptr(), andTerm, usedFields,
                    node.Lambda().Args().Ptr(), labels, *inputs.begin(), renameMap, ordered, ctx);
                if (newJoin != equiJoin.Ptr()) {
                    YQL_CLOG(DEBUG, Core) << "SingleInputPredicatePushdownOverEquiJoin";
                    ret = newJoin;
                    extraPredicate = FuseAndTerms(node.Pos(), andTerms, andTerm, isPg, ctx);
                    break;
                }
            }

            if (inputs.size() == 2) {
                auto newJoin = DecayCrossJoinIntoInner(equiJoin.Ptr(), andTerm,
                    labels, *inputs.begin(), *(++inputs.begin()), row, backRenameMap, parentsMap, ctx);
                if (newJoin != equiJoin.Ptr()) {
                    YQL_CLOG(DEBUG, Core) << "DecayCrossJoinIntoInner";
                    ret = newJoin;
                    extraPredicate = FuseAndTerms(node.Pos(), andTerms, andTerm, isPg, ctx);
                    break;
                }
            }
        }

        if (!ret) {
            return node.Ptr();
        }

        if (extraPredicate) {
            ret = ctx.Builder(node.Pos())
                .Callable(ordered ? "OrderedFilter" : "Filter")
                    .Add(0, std::move(ret))
                    .Lambda(1)
                        .Param("item")
                        .ApplyPartial(node.Lambda().Args().Ptr(), std::move(extraPredicate)).WithNode(row, "item").Seal()
                    .Seal()
                .Seal()
                .Build();
        }

        if (value != &row) {
            TString name = node.Lambda().Body().Ref().Content().StartsWith("Flat") ? "FlatMap" : "Map";
            if (ordered) {
                name.prepend("Ordered");
            }
            ret = ctx.Builder(node.Pos())
                .Callable(name)
                    .Add(0, std::move(ret))
                    .Lambda(1)
                        .Param("item")
                        .ApplyPartial(node.Lambda().Args().Ptr(), std::move(value)).With(0, "item").Seal()
                    .Seal()
                .Seal()
                .Build();
        }

        return ret;
    }

    return node.Ptr();
}

TExprNode::TPtr FlatMapSubsetFields(const TCoFlatMapBase& node, TExprContext& ctx, const TParentsMap& parentsMap) {
    auto it = parentsMap.find(node.Input().Raw());
    YQL_ENSURE(it != parentsMap.cend());
    auto inputParentsCount = it->second.size();

    if (inputParentsCount > 1) {
        return node.Ptr();
    }

    auto itemArg = node.Lambda().Args().Arg(0);
    auto itemType = itemArg.Ref().GetTypeAnn();
    if (itemType->GetKind() != ETypeAnnotationKind::Struct) {
        return node.Ptr();
    }

    auto itemStructType = itemType->Cast<TStructExprType>();
    if (itemStructType->GetSize() == 0) {
        return node.Ptr();
    }

    TSet<TStringBuf> usedFields;
    if (!HaveFieldsSubset(node.Lambda().Body().Ptr(), itemArg.Ref(), usedFields, parentsMap)) {
        return node.Ptr();
    }

    TExprNode::TListType fieldNodes;
    for (auto& item : itemStructType->GetItems()) {
        if (usedFields.contains(item->GetName())) {
            fieldNodes.push_back(ctx.NewAtom(node.Pos(), item->GetName()));
        }
    }

    return Build<TCoFlatMapBase>(ctx, node.Pos())
        .CallableName(node.Ref().Content())
        .Input<TCoExtractMembers>()
            .Input(node.Input())
            .Members()
                .Add(fieldNodes)
                .Build()
            .Build()
        .Lambda()
            .Args({"item"})
            .Body<TExprApplier>()
                .Apply(node.Lambda())
                .With(0, "item")
                .Build()
            .Build()
        .Done()
        .Ptr();
}

TExprNode::TPtr RenameJoinTable(TPositionHandle pos, TExprNode::TPtr table,
    const THashMap<TString, TString>& upstreamTablesRename, TExprContext& ctx)
{
    if (auto renamed = upstreamTablesRename.FindPtr(table->Content())) {
        return ctx.NewAtom(pos, *renamed);
    }

    return table;
}

TExprNode::TPtr RenameEqualityTables(TPositionHandle pos, TExprNode::TPtr columns,
    const THashMap<TString, TString>& upstreamTablesRename, TExprContext& ctx)
{
    TExprNode::TListType newChildren(columns->ChildrenList());
    for (ui32 i = 0; i < newChildren.size(); i += 2) {
        newChildren[i] = RenameJoinTable(pos, newChildren[i], upstreamTablesRename, ctx);
    }

    auto ret = ctx.ChangeChildren(*columns, std::move(newChildren));
    return ret;
}

TExprNode::TPtr RenameJoinTree(TExprNode::TPtr joinTree, const THashMap<TString, TString>& upstreamTablesRename,
    TExprContext& ctx)
{
    TExprNode::TPtr left;
    if (joinTree->Child(1)->IsAtom()) {
        left = RenameJoinTable(joinTree->Pos(), joinTree->Child(1), upstreamTablesRename, ctx);
    }
    else {
        left = RenameJoinTree(joinTree->Child(1), upstreamTablesRename, ctx);
    }

    TExprNode::TPtr right;
    if (joinTree->Child(2)->IsAtom()) {
        right = RenameJoinTable(joinTree->Pos(), joinTree->Child(2), upstreamTablesRename, ctx);
    }
    else {
        right = RenameJoinTree(joinTree->Child(2), upstreamTablesRename, ctx);
    }

    TExprNode::TListType newChildren(joinTree->ChildrenList());
    newChildren[1] = left;
    newChildren[2] = right;
    newChildren[3] = RenameEqualityTables(joinTree->Pos(), joinTree->Child(3), upstreamTablesRename, ctx);
    newChildren[4] = RenameEqualityTables(joinTree->Pos(), joinTree->Child(4), upstreamTablesRename, ctx);

    auto ret = ctx.ChangeChildren(*joinTree, std::move(newChildren));
    return ret;
}

TExprNode::TPtr ReassembleJoinEquality(TExprNode::TPtr columns, const TStringBuf& upstreamLabel,
    const THashMap<TString, TString>& upstreamTablesRename,
    const THashMap<TString, TString>& upstreamColumnsBackRename, TExprContext& ctx)
{
    TExprNode::TListType newChildren(columns->ChildrenList());
    for (ui32 i = 0; i < columns->ChildrenSize(); i += 2) {
        if (columns->Child(i)->Content() != upstreamLabel) {
            continue;
        }

        auto column = columns->Child(i + 1);
        if (auto originalColumn = upstreamColumnsBackRename.FindPtr(column->Content())) {
            TStringBuf part1;
            TStringBuf part2;
            SplitTableName(*originalColumn, part1, part2);
            newChildren[i] = RenameJoinTable(columns->Pos(), ctx.NewAtom(columns->Pos(), part1),
                upstreamTablesRename, ctx);
            newChildren[i + 1] = ctx.NewAtom(columns->Pos(), part2);
        } else {
            TStringBuf part1;
            TStringBuf part2;
            SplitTableName(column->Content(), part1, part2);
            newChildren[i] = RenameJoinTable(columns->Pos(), ctx.NewAtom(columns->Pos(), part1),
                upstreamTablesRename, ctx);
            newChildren[i + 1] = ctx.NewAtom(columns->Pos(), part2);

            return nullptr;
        }
    }

    auto ret = ctx.ChangeChildren(*columns, std::move(newChildren));
    return ret;
}

TExprNode::TPtr FuseJoinTree(TExprNode::TPtr downstreamJoinTree, TExprNode::TPtr upstreamJoinTree, const TStringBuf& upstreamLabel,
    const THashMap<TString, TString>& upstreamTablesRename, const THashMap<TString, TString>& upstreamColumnsBackRename,
    TExprContext& ctx)
{
    TExprNode::TPtr left;
    if (downstreamJoinTree->Child(1)->IsAtom()) {
        if (downstreamJoinTree->Child(1)->Content() != upstreamLabel) {
            left = downstreamJoinTree->Child(1);
        }
        else {
            left = RenameJoinTree(upstreamJoinTree, upstreamTablesRename, ctx);
        }
    }
    else {
        left = FuseJoinTree(downstreamJoinTree->Child(1), upstreamJoinTree, upstreamLabel, upstreamTablesRename,
            upstreamColumnsBackRename, ctx);
        if (!left) {
            return nullptr;
        }
    }

    TExprNode::TPtr right;
    if (downstreamJoinTree->Child(2)->IsAtom()) {
        if (downstreamJoinTree->Child(2)->Content() != upstreamLabel) {
            right = downstreamJoinTree->Child(2);
        }
        else {
            right = RenameJoinTree(upstreamJoinTree, upstreamTablesRename, ctx);
        }
    } else {
        right = FuseJoinTree(downstreamJoinTree->Child(2), upstreamJoinTree, upstreamLabel, upstreamTablesRename,
            upstreamColumnsBackRename, ctx);
        if (!right) {
            return nullptr;
        }
    }

    TExprNode::TListType newChildren(downstreamJoinTree->ChildrenList());
    newChildren[1] = left;
    newChildren[2] = right;
    newChildren[3] = ReassembleJoinEquality(downstreamJoinTree->Child(3), upstreamLabel, upstreamTablesRename,
        upstreamColumnsBackRename, ctx);
    newChildren[4] = ReassembleJoinEquality(downstreamJoinTree->Child(4), upstreamLabel, upstreamTablesRename,
        upstreamColumnsBackRename, ctx);
    if (!newChildren[3] || !newChildren[4]) {
        return nullptr;
    }

    auto ret = ctx.ChangeChildren(*downstreamJoinTree, std::move(newChildren));
    return ret;
}

TExprNode::TPtr FuseEquiJoins(const TExprNode::TPtr& node, ui32 upstreamIndex, TExprContext& ctx) {
    ui32 downstreamInputs = node->ChildrenSize() - 2;
    auto upstreamList = node->Child(upstreamIndex)->Child(0);
    auto upstreamLabel = node->Child(upstreamIndex)->Child(1);
    THashSet<TStringBuf> downstreamLabels;
    for (ui32 i = 0; i < downstreamInputs; ++i) {
        auto label = node->Child(i)->Child(1);
        if (!label->IsAtom()) {
            return node;
        }

        downstreamLabels.insert(label->Content());
    }

    THashMap<TString, TString> upstreamTablesRename; // rename of conflicted upstream tables
    THashMap<TString, TString> upstreamColumnsBackRename; // renamed of columns under upstreamLabel to full name inside upstream
    TMap<TString, TVector<TString>> upstreamColumnsRename;
    ui32 upstreamInputs = upstreamList->ChildrenSize() - 2;
    THashSet<TStringBuf> upstreamLabels;
    for (ui32 i = 0; i < upstreamInputs; ++i) {
        auto label = upstreamList->Child(i)->Child(1);
        if (!label->IsAtom()) {
            return node;
        }

        upstreamLabels.insert(label->Content());
    }

    for (ui32 i = 0; i < upstreamInputs; ++i) {
        auto label = upstreamList->Child(i)->Child(1);
        if (!label->IsAtom()) {
            return node;
        }

        if (downstreamLabels.contains(label->Content())) {
            // fix conflict for labels
            for (ui32 suffix = 1;; ++suffix) {
                auto newName = TString::Join(label->Content(), "_", ToString(suffix));
                if (!downstreamLabels.contains(newName) && !upstreamLabels.contains(newName)) {
                    upstreamTablesRename.insert({ TString(label->Content()) , newName });
                    break;
                }
            }
        }
    }

    TExprNode::TListType equiJoinChildren;
    for (ui32 i = 0; i < downstreamInputs; ++i) {
        if (i != upstreamIndex) {
            equiJoinChildren.push_back(node->Child(i));
        } else {
            // insert the whole upstream inputs
            for (ui32 j = 0; j < upstreamInputs; ++j) {
                auto renamed = upstreamTablesRename.FindPtr(upstreamList->Child(j)->Child(1)->Content());
                if (!renamed) {
                    equiJoinChildren.push_back(upstreamList->Child(j));
                } else {
                    auto pair = ctx.ChangeChild(*upstreamList->Child(j), 1, ctx.NewAtom(node->Pos(), *renamed));
                    equiJoinChildren.push_back(pair);
                }
            }
        }
    }

    auto downstreamJoinTree = node->Child(downstreamInputs);
    auto downstreamSettings = node->Children().back();
    auto upstreamJoinTree = upstreamList->Child(upstreamInputs);
    TExprNode::TListType settingsChildren;

    for (auto& setting : upstreamList->Children().back()->Children()) {
        if (setting->Child(0)->Content() != "rename") {
            // unsupported option to fuse
            return node;
        }

        if (setting->Child(2)->Content().empty()) {
            auto drop = setting->Child(1)->Content();
            TStringBuf part1;
            TStringBuf part2;
            SplitTableName(drop, part1, part2);
            if (auto renamed = upstreamTablesRename.FindPtr(part1)) {
                part1 = *renamed;
            }

            auto newSetting = ctx.ChangeChild(*setting, 1,
                ctx.NewAtom(node->Pos(), TString::Join(part1, ".", part2)));
            settingsChildren.push_back(newSetting);
            continue;
        }

        upstreamColumnsBackRename[TString(setting->Child(2)->Content())] = TString(setting->Child(1)->Content());
        upstreamColumnsRename[TString(setting->Child(1)->Content())].push_back(TString(setting->Child(2)->Content()));
    }

    // fill remaining upstream columns
    for (const auto& item : upstreamList->GetTypeAnn()->Cast<TListExprType>()
        ->GetItemType()->Cast<TStructExprType>()->GetItems()) {
        auto columnName = TString(item->GetName());
        if (upstreamColumnsBackRename.count(columnName)) {
            continue;
        }

        upstreamColumnsRename[columnName].push_back(columnName);
        upstreamColumnsBackRename[columnName] = columnName;
    }

    for (auto& setting : downstreamSettings->Children()) {
        if (setting->Child(0)->Content() != "rename") {
            // unsupported option to fuse
            return node;
        }

        TStringBuf part1;
        TStringBuf part2;
        SplitTableName(setting->Child(1)->Content(), part1, part2);
        if (part1 != upstreamLabel->Content()) {
            settingsChildren.push_back(setting);
            continue;
        }

        if (auto originalName = upstreamColumnsBackRename.FindPtr(part2)) {
            SplitTableName(*originalName, part1, part2);
            if (auto renamed = upstreamTablesRename.FindPtr(part1)) {
                part1 = *renamed;
            }

            upstreamColumnsRename.erase(*originalName);
            auto newSetting = ctx.ChangeChild(*setting, 1, ctx.NewAtom(node->Pos(), TString::Join(part1, '.', part2)));
            settingsChildren.push_back(newSetting);
        } else {
            return node;
        }
    }

    for (auto& x : upstreamColumnsRename) {
        for (auto& y : x.second) {
            TStringBuf part1;
            TStringBuf part2;
            SplitTableName(x.first, part1, part2);
            if (auto renamed = upstreamTablesRename.FindPtr(part1)) {
                part1 = *renamed;
            }

            settingsChildren.push_back(ctx.Builder(node->Pos())
                .List()
                .Atom(0, "rename")
                .Atom(1, TString::Join(part1, ".", part2))
                .Atom(2, TString::Join(upstreamLabel->Content(), ".", y))
                .Seal()
                .Build());
        }
    }

    auto joinTree = FuseJoinTree(downstreamJoinTree, upstreamJoinTree, upstreamLabel->Content(),
        upstreamTablesRename, upstreamColumnsBackRename, ctx);
    if (!joinTree) {
        return node;
    }

    auto newSettings = ctx.NewList(node->Pos(), std::move(settingsChildren));

    equiJoinChildren.push_back(joinTree);
    equiJoinChildren.push_back(newSettings);
    auto ret = ctx.NewCallable(node->Pos(), "EquiJoin", std::move(equiJoinChildren));
    return ret;
}

bool HasOnlyCrossJoins(const TExprNode& joinTree) {
    if (joinTree.IsAtom()) {
        return true;
    }

    YQL_ENSURE(joinTree.Child(0)->IsAtom());
    if (joinTree.Child(0)->Content() != "Cross") {
        return false;
    }

    return HasOnlyCrossJoins(*joinTree.Child(1)) && HasOnlyCrossJoins(*joinTree.Child(2));
}

bool IsRenamingOrPassthroughFlatMap(const TCoFlatMapBase& flatMap, THashMap<TStringBuf, TStringBuf>& renames,
    THashSet<TStringBuf>& outputMembers, bool& isIdentity)
{
    renames.clear();
    outputMembers.clear();
    isIdentity = false;

    auto body = flatMap.Lambda().Body();
    auto arg = flatMap.Lambda().Args().Arg(0);

    if (!IsJustOrSingleAsList(body.Ref())) {
        return false;
    }

    TExprBase outItem(body.Ref().ChildPtr(0));
    if (outItem.Raw() == arg.Raw()) {
        isIdentity = true;
        return true;
    }

    if (auto maybeStruct = outItem.Maybe<TCoAsStruct>()) {
        for (auto child : maybeStruct.Cast()) {
            auto tuple = child.Cast<TCoNameValueTuple>();
            auto value = tuple.Value();
            YQL_ENSURE(outputMembers.insert(tuple.Name().Value()).second);

            if (auto maybeMember = value.Maybe<TCoMember>()) {
                auto member = maybeMember.Cast();
                if (member.Struct().Raw() == arg.Raw()) {
                    TStringBuf oldName = member.Name().Value();
                    TStringBuf newName = tuple.Name().Value();

                    YQL_ENSURE(renames.insert({newName, oldName}).second);
                }
            }
        }
        return true;
    }

    return false;
}

bool IsInputSuitableForPullingOverEquiJoin(const TCoEquiJoinInput& input,
    const THashMap<TStringBuf, THashSet<TStringBuf>>& joinKeysByLabel,
    THashMap<TStringBuf, TStringBuf>& renames, TOptimizeContext& optCtx)
{
    renames.clear();
    YQL_ENSURE(input.Scope().Ref().IsAtom());

    auto maybeFlatMap = TMaybeNode<TCoFlatMapBase>(input.List().Ptr());
    if (!maybeFlatMap) {
        return false;
    }

    auto flatMap = maybeFlatMap.Cast();
    if (flatMap.Lambda().Args().Arg(0).Ref().IsUsedInDependsOn()) {
        return false;
    }

    if (!SilentGetSequenceItemType(flatMap.Input().Ref(), false)) {
        return false;
    }

    if (!optCtx.IsSingleUsage(input) || !optCtx.IsSingleUsage(flatMap)) {
        return false;
    }

    bool isIdentity = false;
    THashSet<TStringBuf> outputMembers;
    if (!IsRenamingOrPassthroughFlatMap(flatMap, renames, outputMembers, isIdentity)) {
        return false;
    }

    if (isIdentity) {
        // all fields are passthrough
        YQL_ENSURE(renames.empty());
        // do not bother pulling identity FlatMap
        return false;
    }

    if (IsTablePropsDependent(flatMap.Lambda().Body().Ref())) {
        renames.clear();
        return false;
    }

    auto keysIt = joinKeysByLabel.find(input.Scope().Ref().Content());
    const auto& joinKeys = (keysIt == joinKeysByLabel.end()) ? THashSet<TStringBuf>() : keysIt->second;

    size_t joinKeysFound = 0;
    bool hasRename = false;
    for (auto it = renames.begin(); it != renames.end();) {
        auto inputName = it->second;
        auto outputName = it->first;
        if (inputName != outputName) {
            hasRename = true;
        }
        YQL_ENSURE(outputMembers.erase(outputName) == 1);
        if (joinKeys.contains(outputName)) {
            joinKeysFound++;
            if (inputName != outputName) {
                it++;
                continue;
            }
        }
        renames.erase(it++);
    }

    if (joinKeysFound != joinKeys.size()) {
        // FlatMap is not renaming/passthrough for some join keys
        renames.clear();
        return false;
    }

    if (!hasRename && outputMembers.empty()) {
        // FlatMap _only_ passes through some subset of input columns
        // do not bother pulling such Flatmap - it will be optimized away later
        renames.clear();
        return false;
    }

    return true;
}

TExprNode::TPtr ApplyRenames(const TExprNode::TPtr& input, const TMap<TStringBuf, TVector<TStringBuf>>& renames,
    const TStructExprType& noRenamesResultType, TStringBuf canaryBaseName, TExprContext& ctx)
{
    TExprNode::TListType asStructArgs;
    for (auto& item : noRenamesResultType.GetItems()) {
        auto memberName = item->GetName();

        TStringBuf tableName;
        TStringBuf columnName;
        SplitTableName(memberName, tableName, columnName);

        if (columnName.find(canaryBaseName, 0) == 0) {
            continue;
        }

        auto it = renames.find(memberName);
        TVector<TStringBuf> passAsIs = { memberName };
        const TVector<TStringBuf>& targets = (it == renames.end()) ? passAsIs : it->second;
        if (targets.empty()) {
            continue;
        }

        auto member = ctx.Builder(input->Pos())
            .Callable("Member")
                .Add(0, input)
                .Atom(1, memberName)
            .Seal()
            .Build();

        for (auto& to : targets) {
            asStructArgs.push_back(
                ctx.Builder(input->Pos())
                    .List()
                        .Atom(0, to)
                        .Add(1, member)
                    .Seal()
                    .Build()
            );
        }
    }

    return ctx.NewCallable(input->Pos(), "AsStruct", std::move(asStructArgs));
}

TExprNode::TPtr ApplyRenamesToJoinKeys(const TExprNode::TPtr& joinKeys,
    const THashMap<TStringBuf, THashMap<TStringBuf, TStringBuf>>& inputJoinKeyRenamesByLabel, TExprContext& ctx)
{
    YQL_ENSURE(joinKeys->ChildrenSize() % 2 == 0);

    TExprNode::TListType newKeys;
    newKeys.reserve(joinKeys->ChildrenSize());

    for (ui32 i = 0; i < joinKeys->ChildrenSize(); i += 2) {
        auto table = joinKeys->ChildPtr(i);
        auto column = joinKeys->ChildPtr(i + 1);

        YQL_ENSURE(table->IsAtom());
        YQL_ENSURE(column->IsAtom());

        auto it = inputJoinKeyRenamesByLabel.find(table->Content());
        if (it != inputJoinKeyRenamesByLabel.end()) {
            auto renameIt = it->second.find(column->Content());
            if (renameIt != it->second.end()) {
                column = ctx.NewAtom(column->Pos(), renameIt->second);
            }
        }

        newKeys.push_back(table);
        newKeys.push_back(column);
    }

    return ctx.NewList(joinKeys->Pos(), std::move(newKeys));
}


TExprNode::TPtr ApplyRenamesToJoinTree(const TExprNode::TPtr& joinTree,
    const THashMap<TStringBuf, THashMap<TStringBuf, TStringBuf>>& inputJoinKeyRenamesByLabel, TExprContext& ctx)
{
    if (joinTree->IsAtom()) {
        return joinTree;
    }

    return ctx.Builder(joinTree->Pos())
        .List()
            .Add(0, joinTree->ChildPtr(0))
            .Add(1, ApplyRenamesToJoinTree(joinTree->ChildPtr(1), inputJoinKeyRenamesByLabel, ctx))
            .Add(2, ApplyRenamesToJoinTree(joinTree->ChildPtr(2), inputJoinKeyRenamesByLabel, ctx))
            .Add(3, ApplyRenamesToJoinKeys(joinTree->ChildPtr(3), inputJoinKeyRenamesByLabel, ctx))
            .Add(4, ApplyRenamesToJoinKeys(joinTree->ChildPtr(4), inputJoinKeyRenamesByLabel, ctx))
            .Add(5, joinTree->ChildPtr(5))
        .Seal()
        .Build();
}

const TTypeAnnotationNode* GetCanaryOutputType(const TStructExprType& outputType, TStringBuf fullCanaryName) {
    auto maybeIndex = outputType.FindItem(fullCanaryName);
    if (!maybeIndex) {
        return nullptr;
    }
    return outputType.GetItems()[*maybeIndex]->GetItemType();
}

TExprNode::TPtr BuildOutputFlattenMembersArg(const TCoEquiJoinInput& input, const TExprNode::TPtr& inputArg,
    const TString& canaryName, const TStructExprType& canaryResultTypeWithoutRenames, bool keepSys, TExprContext& ctx)
{
    YQL_ENSURE(input.Scope().Ref().IsAtom());
    TStringBuf label = input.Scope().Ref().Content();

    auto flatMap = input.List().Cast<TCoFlatMapBase>();
    auto lambda = flatMap.Lambda();
    YQL_ENSURE(IsJustOrSingleAsList(lambda.Body().Ref()));
    auto strippedLambdaBody = lambda.Body().Ref().HeadPtr();

    const TString labelPrefix = TString::Join(label, ".");
    const TString fullCanaryName = FullColumnName(label, canaryName);

    const TTypeAnnotationNode* canaryOutType = GetCanaryOutputType(canaryResultTypeWithoutRenames, fullCanaryName);
    if (!canaryOutType) {
        // canary didn't survive join
        return {};
    }

    auto flatMapInputItem = GetSequenceItemType(flatMap.Input(), false);
    auto flatMapOutputItem = GetSequenceItemType(flatMap, false);

    auto myStruct = ctx.Builder(input.Pos())
        .Callable("DivePrefixMembers")
            .Add(0, inputArg)
            .List(1)
                .Atom(0, labelPrefix)
            .Seal()
        .Seal()
        .Build();

    if (canaryOutType->GetKind() == ETypeAnnotationKind::Data) {
        YQL_ENSURE(canaryOutType->Cast<TDataExprType>()->GetSlot() == EDataSlot::Bool);
        // our input passed as-is
        return ctx.Builder(input.Pos())
            .List()
                .Atom(0, labelPrefix)
                .ApplyPartial(1, lambda.Args().Ptr(), std::move(strippedLambdaBody))
                    .With(0, std::move(myStruct))
                .Seal()
            .Seal()
            .Build();
    }

    YQL_ENSURE(canaryOutType->GetKind() == ETypeAnnotationKind::Optional);

    TExprNode::TListType membersForCheck;
    auto flatMapInputItems = flatMapInputItem->Cast<TStructExprType>()->GetItems();
    if (!keepSys) {
        EraseIf(flatMapInputItems, [](const TItemExprType* item) { return item->GetName().StartsWith("_yql_sys_"); });
    }
    flatMapInputItems.push_back(ctx.MakeType<TItemExprType>(canaryName, ctx.MakeType<TDataExprType>(EDataSlot::Bool)));
    for (auto& item : flatMapInputItems) {
        if (item->GetItemType()->GetKind() != ETypeAnnotationKind::Optional) {
            membersForCheck.emplace_back(ctx.NewAtom(input.Pos(), item->GetName()));
        }
    }

    auto checkedMembersList = ctx.NewList(input.Pos(), std::move(membersForCheck));

    return ctx.Builder(input.Pos())
        .List()
            .Atom(0, labelPrefix)
            .Callable(1, "IfPresent")
                .Callable(0, "FilterNullMembers")
                    .Callable(0, "AssumeAllMembersNullableAtOnce")
                        .Callable(0, "Just")
                            .Add(0, std::move(myStruct))
                        .Seal()
                    .Seal()
                    .Add(1, std::move(checkedMembersList))
                .Seal()
                .Lambda(1)
                    .Param("canaryInput")
                    .Callable("FlattenMembers")
                        .List(0)
                            .Atom(0, "")
                            .Callable(1, "Just")
                                .ApplyPartial(0, lambda.Args().Ptr(), std::move(strippedLambdaBody))
                                    .With(0)
                                        .Callable("RemoveMember")
                                            .Arg(0, "canaryInput")
                                            .Atom(1, canaryName)
                                        .Seal()
                                    .Done()
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                .Seal()
                .Callable(2, "FlattenMembers")
                    .List(0)
                        .Atom(0, "")
                        .Callable(1, "Nothing")
                            .Add(0, ExpandType(input.Pos(), *ctx.MakeType<TOptionalExprType>(flatMapOutputItem), ctx))
                        .Seal()
                    .Seal()
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr PullUpFlatMapOverEquiJoin(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    if (!optCtx.Types->PullUpFlatMapOverJoin) {
        return node;
    }

    YQL_ENSURE(node->ChildrenSize() >= 4);
    auto inputsCount = ui32(node->ChildrenSize() - 2);

    auto joinTree = node->ChildPtr(inputsCount);
    if (HasOnlyCrossJoins(*joinTree)) {
        return node;
    }

    bool keepSys = false;
    auto settings = node->ChildPtr(inputsCount + 1);
    for (auto& child : settings->Children()) {
        if (child->Child(0)->Content() == "flatten") {
            return node;
        }
        if (child->Child(0)->Content() == "keep_sys") {
            keepSys = true;
        }
    }

    static const TStringBuf canaryBaseName = "_yql_canary_";

    THashMap<TStringBuf, THashSet<TStringBuf>> joinKeysByLabel = CollectEquiJoinKeyColumnsByLabel(*joinTree);
    const auto renames = LoadJoinRenameMap(*settings);

    TVector<ui32> toPull;
    TJoinLabels canaryLabels;
    TJoinLabels actualLabels;
    THashMap<TStringBuf, THashMap<TStringBuf, TStringBuf>> inputJoinKeyRenamesByLabel;
    for (ui32 i = 0; i < inputsCount; ++i) {
        TCoEquiJoinInput input(node->ChildPtr(i));

        if (!input.Scope().Ref().IsAtom()) {
            return node;
        }

        const TTypeAnnotationNode* itemType = input.List().Ref().GetTypeAnn()->Cast<TListExprType>()->GetItemType();
        auto structType = itemType->Cast<TStructExprType>();
        for (auto& si : structType->GetItems()) {
            if (si->GetName().find(canaryBaseName, 0) == 0) {
                // EquiJoin already processed
                return node;
            }
        }

        auto err = actualLabels.Add(ctx, *input.Scope().Ptr(), structType);
        YQL_ENSURE(!err);

        auto label = input.Scope().Ref().Content();


        if (IsInputSuitableForPullingOverEquiJoin(input, joinKeysByLabel, inputJoinKeyRenamesByLabel[label], optCtx)) {
            auto flatMap = input.List().Cast<TCoFlatMapBase>();

            auto flatMapInputItem = GetSequenceItemType(flatMap.Input(), false);
            auto structItems = flatMapInputItem->Cast<TStructExprType>()->GetItems();
            if (!keepSys) {
                EraseIf(structItems, [](const TItemExprType* item) { return item->GetName().StartsWith("_yql_sys_"); });
            }

            TString canaryName = TStringBuilder() << canaryBaseName << i;
            structItems.push_back(ctx.MakeType<TItemExprType>(canaryName, ctx.MakeType<TDataExprType>(EDataSlot::Bool)));
            structType = ctx.MakeType<TStructExprType>(structItems);

            YQL_CLOG(DEBUG, Core) << "Will pull up EquiJoin input #" << i;
            toPull.push_back(i);
        }

        err = canaryLabels.Add(ctx, *input.Scope().Ptr(), structType);
        YQL_ENSURE(!err);
    }

    if (toPull.empty()) {
        return node;
    }

    const TStructExprType* canaryResultType = nullptr;
    const TStructExprType* noRenamesResultType = nullptr;
    const auto settingsWithoutRenames = RemoveSetting(*settings, "rename", ctx);
    const auto joinTreeWithInputRenames = ApplyRenamesToJoinTree(joinTree, inputJoinKeyRenamesByLabel, ctx);


    {
        TJoinOptions options;
        auto status = ValidateEquiJoinOptions(node->Pos(), *settingsWithoutRenames, options, ctx);
        YQL_ENSURE(status == IGraphTransformer::TStatus::Ok);

        status = EquiJoinAnnotation(node->Pos(), canaryResultType, canaryLabels,
                                         *joinTreeWithInputRenames, options, ctx);
        YQL_ENSURE(status == IGraphTransformer::TStatus::Ok);

        status = EquiJoinAnnotation(node->Pos(), noRenamesResultType, actualLabels,
                                    *joinTree, options, ctx);
        YQL_ENSURE(status == IGraphTransformer::TStatus::Ok);
    }



    TExprNode::TListType newEquiJoinArgs;
    newEquiJoinArgs.reserve(node->ChildrenSize());

    TExprNode::TListType flattenMembersArgs;

    auto afterJoinArg = ctx.NewArgument(node->Pos(), "joinOut");

    for (ui32 i = 0, j = 0; i < inputsCount; ++i) {
        TCoEquiJoinInput input(node->ChildPtr(i));

        TStringBuf label = input.Scope().Ref().Content();
        TString labelPrefix = TString::Join(label, ".");

        if (j < toPull.size() && i == toPull[j]) {
            j++;


            const TString canaryName = TStringBuilder() << canaryBaseName << i;
            const TString fullCanaryName = FullColumnName(label, canaryName);

            TCoFlatMapBase flatMap = input.List().Cast<TCoFlatMapBase>();

            const TTypeAnnotationNode* canaryOutType = GetCanaryOutputType(*canaryResultType, fullCanaryName);
            if (canaryOutType && canaryOutType->GetKind() == ETypeAnnotationKind::Optional) {
                // remove leading flatmap from input and launch canary
                newEquiJoinArgs.push_back(
                    ctx.Builder(input.Pos())
                        .List()
                            .Callable(0, flatMap.CallableName())
                                .Add(0, flatMap.Input().Ptr())
                                .Lambda(1)
                                    .Param("item")
                                    .Callable("Just")
                                        .Callable(0, "AddMember")
                                            .Arg(0, "item")
                                            .Atom(1, canaryName)
                                            .Callable(2, "Bool")
                                                .Atom(0, "true")
                                            .Seal()
                                        .Seal()
                                    .Seal()
                                .Seal()
                            .Seal()
                            .Add(1, input.Scope().Ptr())
                        .Seal()
                        .Build()
                );
            } else {
                // just remove leading flatmap from input
                newEquiJoinArgs.push_back(
                    ctx.Builder(input.Pos())
                        .List()
                            .Add(0, flatMap.Input().Ptr())
                            .Add(1, input.Scope().Ptr())
                        .Seal()
                        .Build()
                );
            }

            auto flattenMembersArg = BuildOutputFlattenMembersArg(input, afterJoinArg, canaryName, *canaryResultType, keepSys, ctx);
            if (flattenMembersArg) {
                flattenMembersArgs.push_back(flattenMembersArg);
            }
        } else {
            flattenMembersArgs.push_back(ctx.Builder(input.Pos())
                .List()
                    .Atom(0, labelPrefix)
                    .Callable(1, "DivePrefixMembers")
                        .Add(0, afterJoinArg)
                        .List(1)
                            .Atom(0, labelPrefix)
                        .Seal()
                    .Seal()
                .Seal()
                .Build());
            newEquiJoinArgs.push_back(input.Ptr());
        }
    }

    newEquiJoinArgs.push_back(joinTreeWithInputRenames);
    newEquiJoinArgs.push_back(settingsWithoutRenames);

    auto newEquiJoin = ctx.NewCallable(node->Pos(), "EquiJoin", std::move(newEquiJoinArgs));

    auto flattenMembers = flattenMembersArgs.empty() ? afterJoinArg :
                          ctx.NewCallable(node->Pos(), "FlattenMembers", std::move(flattenMembersArgs));

    auto newLambdaBody = ctx.Builder(node->Pos())
        .Callable("Just")
            .Add(0, ApplyRenames(flattenMembers, renames, *noRenamesResultType, canaryBaseName, ctx))
        .Seal()
        .Build();

    auto newLambda = ctx.NewLambda(node->Pos(), ctx.NewArguments(node->Pos(), { afterJoinArg }), std::move(newLambdaBody));

    return ctx.NewCallable(node->Pos(), "FlatMap", { newEquiJoin, newLambda });
}

TExprNode::TPtr OptimizeFromFlow(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    if (!optCtx.IsSingleUsage(node->Head())) {
        return node;
    }

    if (node->Head().IsCallable("ToFlow") &&
        node->Head().Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::Stream) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Content() << " with " << node->Head().Content();
        return node->Head().HeadPtr();
    }

    if (node->Head().IsCallable("ToFlow") &&
        node->Head().Head().GetTypeAnn()->GetKind() == ETypeAnnotationKind::List) {
        YQL_CLOG(DEBUG, Core) << "Replace  " << node->Content() << " with Iterator";

        return Build<TCoIterator>(ctx, node->Pos())
            .List(node->HeadPtr()->HeadPtr())
            .Done()
            .Ptr();
    }

    return node;
}

TExprNode::TPtr OptimizeCollect(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    if (!optCtx.IsSingleUsage(node->Head())) {
        return node;
    }

    if (node->Head().IsCallable({"ToFlow", "FromFlow"}) &&
        node->Head().Head().GetTypeAnn()->GetKind() != ETypeAnnotationKind::Optional) {
        YQL_CLOG(DEBUG, Core) << "Drop " << node->Head().Content() <<  " under " << node->Content();
        return ctx.ChangeChildren(*node, node->Head().ChildrenList());
    }

    return node;
}

}

void RegisterCoFlowCallables2(TCallableOptimizerMap& map) {
    using namespace std::placeholders;

    map["FromFlow"] = std::bind(&OptimizeFromFlow, _1, _2, _3);
    map["Collect"] = std::bind(&OptimizeCollect, _1, _2, _3);

    map["FlatMap"] = map["OrderedFlatMap"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoFlatMapBase self(node);
        if (!optCtx.IsSingleUsage(self.Input().Ref())) {
            return node;
        }

        if (self.Input().Ref().IsCallable("EquiJoin")) {
            auto ret = FlatMapOverEquiJoin(self, ctx, *optCtx.ParentsMap);
            if (ret != node) {
                YQL_CLOG(DEBUG, Core) << node->Content() << "OverEquiJoin";
                return ret;
            }
        }

        if (self.Input().Ref().IsCallable(TCoGroupingCore::CallableName())) {
            auto groupingCore = self.Input().Cast<TCoGroupingCore>();
            const TExprNode* extract = nullptr;
            // Find pattern: (FlatMap (GroupingCore ...) (lambda (x) ( ... (ExtractMembers (Nth x '1) ...))))
            const auto arg = self.Lambda().Args().Arg(0).Raw();
            if (const auto parents = optCtx.ParentsMap->find(arg); parents != optCtx.ParentsMap->cend()) {
                for (const auto& parent : parents->second) {
                    if (parent->IsCallable(TCoNth::CallableName()) && &parent->Head() == arg && parent->Tail().Content() == "1") {
                        if (const auto nthParents = optCtx.ParentsMap->find(parent); nthParents != optCtx.ParentsMap->cend()) {
                            if (nthParents->second.size() == 1 && (*nthParents->second.begin())->IsCallable(TCoExtractMembers::CallableName())) {
                                extract = *nthParents->second.begin();
                                break;
                            }
                        }
                    }
                }
            }
            if (extract) {
                if (const auto handler = groupingCore.ConvertHandler()) {
                    auto newBody = Build<TCoCastStruct>(ctx, handler.Cast().Body().Pos())
                        .Struct(handler.Cast().Body())
                        .Type(ExpandType(handler.Cast().Body().Pos(), *GetSeqItemType(extract->GetTypeAnn()), ctx))
                        .Done();

                    groupingCore = Build<TCoGroupingCore>(ctx, groupingCore.Pos())
                        .InitFrom(groupingCore)
                        .ConvertHandler()
                            .Args({"item"})
                            .Body<TExprApplier>()
                                .Apply(newBody)
                                .With(handler.Cast().Args().Arg(0), "item")
                            .Build()
                        .Build()
                        .Done();

                    YQL_CLOG(DEBUG, Core) << "Pull out " << extract->Content() << " from " << node->Content() << " to " << groupingCore.Ref().Content() << " handler";
                    return Build<TCoFlatMapBase>(ctx, node->Pos())
                        .CallableName(node->Content())
                        .Input(groupingCore)
                        .Lambda(ctx.DeepCopyLambda(self.Lambda().Ref()))
                        .Done().Ptr();
                }

                std::map<std::string_view, TExprNode::TPtr> usedFields;
                auto fields = extract->Tail().ChildrenList();
                std::for_each(fields.cbegin(), fields.cend(), [&](const TExprNode::TPtr& field) { usedFields.emplace(field->Content(), field); });

                if (HaveFieldsSubset(groupingCore.KeyExtractor().Body().Ptr(), groupingCore.KeyExtractor().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
                    && !usedFields.empty()
                    && HaveFieldsSubset(groupingCore.GroupSwitch().Body().Ptr(), groupingCore.GroupSwitch().Args().Arg(1).Ref(), usedFields, *optCtx.ParentsMap, false)
                    && !usedFields.empty()
                    && usedFields.size() < GetSeqItemType(groupingCore.Input().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetSize()) {
                    if (usedFields.size() != fields.size()) {
                        fields.reserve(usedFields.size());
                        fields.clear();
                        std::transform(usedFields.begin(), usedFields.end(), std::back_inserter(fields),
                            [](std::pair<const std::string_view, TExprNode::TPtr>& item){ return std::move(item.second); });
                    }

                    YQL_CLOG(DEBUG, Core) << "Pull out " << extract->Content() << " from " << node->Content() << " to " << groupingCore.Ref().Content() << " input";
                    return Build<TCoFlatMapBase>(ctx, node->Pos())
                        .CallableName(node->Content())
                        .Input<TCoGroupingCore>()
                            .Input<TCoExtractMembers>()
                                .Input(groupingCore.Input())
                                .Members()
                                    .Add(std::move(fields))
                                .Build()
                            .Build()
                            .GroupSwitch(ctx.DeepCopyLambda(groupingCore.GroupSwitch().Ref()))
                            .KeyExtractor(ctx.DeepCopyLambda(groupingCore.KeyExtractor().Ref()))
                        .Build()
                        .Lambda(ctx.DeepCopyLambda(self.Lambda().Ref()))
                        .Done().Ptr();
                }
            }
        }

        if (self.Input().Ref().IsCallable("Take") || self.Input().Ref().IsCallable("Skip")
            || self.Input().Maybe<TCoExtendBase>()) {

            auto& arg = self.Lambda().Args().Arg(0).Ref();
            auto body = self.Lambda().Body().Ptr();
            TSet<TStringBuf> usedFields;
            if (HaveFieldsSubset(body, arg, usedFields, *optCtx.ParentsMap)) {
                YQL_CLOG(DEBUG, Core) << "FieldsSubset in " << node->Content() << " over " << self.Input().Ref().Content();
                TSet<TString> fields;
                for (auto& x : usedFields) {
                    fields.emplace(TString(x));
                }

                TExprNode::TListType filteredInputs;
                for (ui32 index = 0; index < self.Input().Ref().ChildrenSize(); ++index) {
                    auto x = self.Input().Ref().ChildPtr(index);
                    if (!self.Input().Maybe<TCoExtendBase>() && index > 0) {
                        filteredInputs.push_back(x);
                        continue;
                    }

                    filteredInputs.push_back(FilterByFields(node->Pos(), x, fields, ctx, false));
                }

                auto newInput = ctx.ChangeChildren(self.Input().Ref(), std::move(filteredInputs));
                return ctx.Builder(node->Pos())
                    .Callable(node->Content())
                        .Add(0, newInput)
                        .Lambda(1)
                            .Param("item")
                            .Apply(self.Lambda().Ptr()).With(0, "item").Seal()
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        auto ret = FlatMapSubsetFields(self, ctx, *optCtx.ParentsMap);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << node->Content() << "SubsetFields";
            return ret;
        }

        return node;
    };

    map[TCoGroupingCore::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoGroupingCore self(node);
        if (!optCtx.IsSingleUsage(self.Input().Ref())) {
            return node;
        }

        if (!self.ConvertHandler()) {
            return node;
        }

        std::map<std::string_view, TExprNode::TPtr> usedFields;
        if (HaveFieldsSubset(self.ConvertHandler().Cast().Body().Ptr(), self.ConvertHandler().Cast().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && HaveFieldsSubset(self.KeyExtractor().Body().Ptr(), self.KeyExtractor().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && HaveFieldsSubset(self.GroupSwitch().Body().Ptr(), self.GroupSwitch().Args().Arg(1).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && usedFields.size() < GetSeqItemType(self.Input().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetSize())
        {
            TExprNode::TListType fields;
            fields.reserve(usedFields.size());
            std::transform(usedFields.begin(), usedFields.end(), std::back_inserter(fields),
                [](std::pair<const std::string_view, TExprNode::TPtr>& item){ return std::move(item.second); });

            YQL_CLOG(DEBUG, Core) << node->Content() << "SubsetFields";
            return Build<TCoGroupingCore>(ctx, node->Pos())
                .Input<TCoExtractMembers>()
                    .Input(self.Input())
                    .Members()
                        .Add(std::move(fields))
                    .Build()
                .Build()
                .GroupSwitch(ctx.DeepCopyLambda(self.GroupSwitch().Ref()))
                .KeyExtractor(ctx.DeepCopyLambda(self.KeyExtractor().Ref()))
                .ConvertHandler(ctx.DeepCopyLambda(self.ConvertHandler().Ref()))
                .Done().Ptr();
        }
        return node;
    };

    map["CombineByKey"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoCombineByKey self(node);
        if (!optCtx.IsSingleUsage(self.Input().Ref())) {
            return node;
        }

        auto itemArg = self.PreMapLambda().Args().Arg(0);
        auto itemType = itemArg.Ref().GetTypeAnn();
        if (itemType->GetKind() != ETypeAnnotationKind::Struct) {
            return node;
        }

        auto itemStructType = itemType->Cast<TStructExprType>();
        if (itemStructType->GetSize() == 0) {
            return node;
        }

        TSet<TStringBuf> usedFields;
        if (!HaveFieldsSubset(self.PreMapLambda().Body().Ptr(), itemArg.Ref(), usedFields, *optCtx.ParentsMap)) {
            return node;
        }

        TExprNode::TPtr newInput;
        if (self.Input().Ref().IsCallable("Take") || self.Input().Ref().IsCallable("Skip") || self.Input().Maybe<TCoExtendBase>()) {
            TSet<TString> fields;
            for (auto& x : usedFields) {
                fields.emplace(TString(x));
            }

            TExprNode::TListType filteredInputs;
            for (ui32 index = 0; index < self.Input().Ref().ChildrenSize(); ++index) {
                auto x = self.Input().Ref().ChildPtr(index);
                if (!self.Input().Maybe<TCoExtendBase>() && index > 0) {
                    filteredInputs.push_back(x);
                    continue;
                }

                filteredInputs.push_back(FilterByFields(node->Pos(), x, fields, ctx, false));
            }

            YQL_CLOG(DEBUG, Core) << "FieldsSubset in " << node->Content() << " over " << self.Input().Ref().Content();
            newInput = ctx.ChangeChildren(self.Input().Ref(), std::move(filteredInputs));
        }
        else {
            TExprNode::TListType fieldNodes;
            for (auto& item : itemStructType->GetItems()) {
                if (usedFields.contains(item->GetName())) {
                    fieldNodes.push_back(ctx.NewAtom(self.Pos(), item->GetName()));
                }
            }

            YQL_CLOG(DEBUG, Core) << node->Content() << "SubsetFields";
            newInput = Build<TCoExtractMembers>(ctx, self.Input().Pos())
                .Input(self.Input())
                .Members()
                    .Add(fieldNodes)
                .Build()
                .Done()
                .Ptr();
        }

        return Build<TCoCombineByKey>(ctx, self.Pos())
            .Input(newInput)
            .PreMapLambda(ctx.DeepCopyLambda(self.PreMapLambda().Ref()))
            .KeySelectorLambda(ctx.DeepCopyLambda(self.KeySelectorLambda().Ref()))
            .InitHandlerLambda(ctx.DeepCopyLambda(self.InitHandlerLambda().Ref()))
            .UpdateHandlerLambda(ctx.DeepCopyLambda(self.UpdateHandlerLambda().Ref()))
            .FinishHandlerLambda(ctx.DeepCopyLambda(self.FinishHandlerLambda().Ref()))
            .Done()
            .Ptr();
    };

    map["EquiJoin"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        ui32 inputsCount = node->ChildrenSize() - 2;
        for (ui32 i = 0; i < inputsCount; ++i) {
            if (node->Child(i)->Child(0)->IsCallable("EquiJoin") &&
                optCtx.IsSingleUsage(*node->Child(i)) &&
                optCtx.IsSingleUsage(*node->Child(i)->Child(0))) {
                auto ret = FuseEquiJoins(node, i, ctx);
                if (ret != node) {
                    YQL_CLOG(DEBUG, Core) << "FuseEquiJoins";
                    return ret;
                }
            }
        }

        auto ret = PullUpFlatMapOverEquiJoin(node, ctx, optCtx);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "PullUpFlatMapOverEquiJoin";
            return ret;
        }

        return node;
    };

    map["ExtractMembers"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoExtractMembers self(node);
        if (!optCtx.IsSingleUsage(self.Input())) {
            return node;
        }

        if (self.Input().Maybe<TCoTake>()) {
            if (auto res = ApplyExtractMembersToTake(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoSkip>()) {
            if (auto res = ApplyExtractMembersToSkip(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoSkipNullMembers>()) {
            if (auto res = ApplyExtractMembersToSkipNullMembers(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoFilterNullMembers>()) {
            if (auto res = ApplyExtractMembersToFilterNullMembers(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoSortBase>()) {
            if (auto res = ApplyExtractMembersToSort(self.Input().Ptr(), self.Members().Ptr(), *optCtx.ParentsMap, ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoAssumeUnique>()) {
            if (auto res = ApplyExtractMembersToAssumeUnique(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoTopBase>()) {
            if (auto res = ApplyExtractMembersToTop(self.Input().Ptr(), self.Members().Ptr(), *optCtx.ParentsMap, ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoExtendBase>()) {
            if (auto res = ApplyExtractMembersToExtend(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoEquiJoin>()) {
            if (auto res = ApplyExtractMembersToEquiJoin(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoFlatMapBase>()) {
            if (auto res = ApplyExtractMembersToFlatMap(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoPartitionByKey>()) {
            if (auto res = ApplyExtractMembersToPartitionByKey(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoCalcOverWindowBase>() || self.Input().Maybe<TCoCalcOverWindowGroup>()) {
            if (auto res = ApplyExtractMembersToCalcOverWindow(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoAggregate>()) {
            if (auto res = ApplyExtractMembersToAggregate(self.Input().Ptr(), self.Members().Ptr(), *optCtx.ParentsMap, ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoChopper>()) {
            if (auto res = ApplyExtractMembersToChopper(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoCollect>()) {
            if (auto res = ApplyExtractMembersToCollect(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoMapJoinCore>()) {
            if (auto res = ApplyExtractMembersToMapJoinCore(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        if (self.Input().Maybe<TCoMapNext>()) {
            if (auto res = ApplyExtractMembersToMapNext(self.Input().Ptr(), self.Members().Ptr(), ctx, {})) {
                return res;
            }
            return node;
        }

        return node;
    };

    map[TCoChopper::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        const TCoChopper chopper(node);
        const auto arg = chopper.Handler().Args().Arg(1).Raw();
        if (const auto parents = optCtx.ParentsMap->find(arg); parents != optCtx.ParentsMap->cend()
            && parents->second.size() == 1
            && (*parents->second.begin())->IsCallable(TCoExtractMembers::CallableName())
            && arg == &(*parents->second.begin())->Head())
        {
            const auto extract = *parents->second.begin();
            std::map<std::string_view, TExprNode::TPtr> usedFields;
            auto fields = extract->Tail().ChildrenList();
            std::for_each(fields.cbegin(), fields.cend(), [&](const TExprNode::TPtr& field){ usedFields.emplace(field->Content(), field); });

            if (HaveFieldsSubset(chopper.KeyExtractor().Body().Ptr(), chopper.KeyExtractor().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false) && !usedFields.empty() &&
                HaveFieldsSubset(chopper.GroupSwitch().Body().Ptr(), chopper.GroupSwitch().Args().Arg(1).Ref(), usedFields, *optCtx.ParentsMap, false) && !usedFields.empty() &&
                usedFields.size() < GetSeqItemType(chopper.Input().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetSize()) {
                if (usedFields.size() != fields.size()) {
                    fields.reserve(usedFields.size());
                    fields.clear();
                    std::transform(usedFields.begin(), usedFields.end(), std::back_inserter(fields),
                        [](std::pair<const std::string_view, TExprNode::TPtr>& item){ return std::move(item.second); });
                }

                YQL_CLOG(DEBUG, Core) << "Pull out " << extract->Content() << " from " << node->Content();
                return Build<TCoChopper>(ctx, chopper.Pos())
                    .Input<TCoExtractMembers>()
                        .Input(chopper.Input())
                        .Members().Add(std::move(fields)).Build()
                        .Build()
                    .KeyExtractor(ctx.DeepCopyLambda(chopper.KeyExtractor().Ref()))
                    .GroupSwitch(ctx.DeepCopyLambda(chopper.GroupSwitch().Ref()))
                    .Handler(ctx.DeepCopyLambda(chopper.Handler().Ref()))
                    .Done().Ptr();
            }
        }
        return node;
    };

    map["WindowTraits"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        auto structType = node->Child(0)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
        TSet<TStringBuf> usedFields;
        auto initLambda = node->Child(1);
        auto updateLambda = node->Child(2);
        TSet<TStringBuf> lambdaSubset;
        if (!HaveFieldsSubset(initLambda->ChildPtr(1), *initLambda->Child(0)->Child(0), lambdaSubset, *optCtx.ParentsMap)) {
            return node;
        }

        usedFields.insert(lambdaSubset.cbegin(), lambdaSubset.cend());
        if (!HaveFieldsSubset(updateLambda->ChildPtr(1), *updateLambda->Child(0)->Child(0), lambdaSubset, *optCtx.ParentsMap)) {
            return node;
        }

        usedFields.insert(lambdaSubset.cbegin(), lambdaSubset.cend());
        if (usedFields.size() == structType->GetSize()) {
            return node;
        }

        TVector<const TItemExprType*> subsetItems;
        for (const auto& item : structType->GetItems()) {
            if (usedFields.contains(item->GetName())) {
                subsetItems.push_back(item);
            }
        }

        auto subsetType = ctx.MakeType<TStructExprType>(subsetItems);
        YQL_CLOG(DEBUG, Core) << "FieldSubset for WindowTraits";
        return ctx.Builder(node->Pos())
            .Callable("WindowTraits")
                .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                .Add(1, ctx.DeepCopyLambda(*node->Child(1)))
                .Add(2, ctx.DeepCopyLambda(*node->Child(2)))
                .Add(3, ctx.DeepCopyLambda(*node->Child(3)))
                .Add(4, ctx.DeepCopyLambda(*node->Child(4)))
                .Add(5, node->Child(5)->IsLambda() ? ctx.DeepCopyLambda(*node->Child(5)) : node->ChildPtr(5))
            .Seal()
            .Build();
    };

    map[TCoHoppingTraits::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoHoppingTraits self(node);

        auto structType = node->Child(0)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();

        const auto lambdaBody = self.TimeExtractor().Body().Ptr();
        const auto& arg = self.TimeExtractor().Args().Arg(0).Ref();

        TSet<TStringBuf> usedFields;
        if (!HaveFieldsSubset(lambdaBody, arg, usedFields, *optCtx.ParentsMap)) {
            return node;
        }

        if (usedFields.size() == structType->GetSize()) {
            return node;
        }

        TVector<const TItemExprType*> subsetItems;
        for (const auto& item : structType->GetItems()) {
            if (usedFields.contains(item->GetName())) {
                subsetItems.push_back(item);
            }
        }

        auto subsetType = ctx.MakeType<TStructExprType>(subsetItems);
        YQL_CLOG(DEBUG, Core) << "FieldSubset for HoppingTraits";
        return Build<TCoHoppingTraits>(ctx, node->Pos())
            .ItemType(ExpandType(node->Pos(), *subsetType, ctx))
            .TimeExtractor(ctx.DeepCopyLambda(self.TimeExtractor().Ref()))
            .Hop(self.Hop())
            .Interval(self.Interval())
            .Delay(self.Delay())
            .DataWatermarks(self.DataWatermarks())
            .Done().Ptr();
    };

    map["AggregationTraits"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        auto type = node->Child(0)->GetTypeAnn()->Cast<TTypeExprType>()->GetType();
        if (type->GetKind() != ETypeAnnotationKind::Struct) {
            // usually distinct, type of column is used instead
            return node;
        }

        auto structType = type->Cast<TStructExprType>();
        TSet<TStringBuf> usedFields;
        auto initLambda = node->Child(1);
        auto updateLambda = node->Child(2);
        TSet<TStringBuf> lambdaSubset;
        if (!HaveFieldsSubset(initLambda->ChildPtr(1), *initLambda->Child(0)->Child(0), lambdaSubset, *optCtx.ParentsMap)) {
            return node;
        }

        usedFields.insert(lambdaSubset.cbegin(), lambdaSubset.cend());
        if (!HaveFieldsSubset(updateLambda->ChildPtr(1), *updateLambda->Child(0)->Child(0), lambdaSubset, *optCtx.ParentsMap)) {
            return node;
        }

        usedFields.insert(lambdaSubset.cbegin(), lambdaSubset.cend());
        if (usedFields.size() == structType->GetSize()) {
            return node;
        }

        TVector<const TItemExprType*> subsetItems;
        for (const auto& item : structType->GetItems()) {
            if (usedFields.contains(item->GetName())) {
                subsetItems.push_back(item);
            }
        }

        auto subsetType = ctx.MakeType<TStructExprType>(subsetItems);
        YQL_CLOG(DEBUG, Core) << "FieldSubset for AggregationTraits";
        return ctx.Builder(node->Pos())
            .Callable("AggregationTraits")
                .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                .Add(1, ctx.DeepCopyLambda(*node->Child(1)))
                .Add(2, ctx.DeepCopyLambda(*node->Child(2)))
                .Add(3, ctx.DeepCopyLambda(*node->Child(3)))
                .Add(4, ctx.DeepCopyLambda(*node->Child(4)))
                .Add(5, ctx.DeepCopyLambda(*node->Child(5)))
                .Add(6, ctx.DeepCopyLambda(*node->Child(6)))
                .Add(7, node->Child(7)->IsLambda() ? ctx.DeepCopyLambda(*node->Child(7)) : node->ChildPtr(7))
            .Seal()
            .Build();
    };

    map["AggApply"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        auto type = node->Child(1)->GetTypeAnn()->Cast<TTypeExprType>()->GetType();
        if (type->GetKind() != ETypeAnnotationKind::Struct) {
            // usually distinct, type of column is used instead
            return node;
        }

        auto structType = type->Cast<TStructExprType>();
        TSet<TStringBuf> usedFields;
        auto extractor = node->Child(2);
        TSet<TStringBuf> lambdaSubset;
        if (!HaveFieldsSubset(extractor->ChildPtr(1), *extractor->Child(0)->Child(0), lambdaSubset, *optCtx.ParentsMap)) {
            return node;
        }

        usedFields.insert(lambdaSubset.cbegin(), lambdaSubset.cend());
        if (usedFields.size() == structType->GetSize()) {
            return node;
        }

        TVector<const TItemExprType*> subsetItems;
        for (const auto& item : structType->GetItems()) {
            if (usedFields.contains(item->GetName())) {
                subsetItems.push_back(item);
            }
        }

        auto subsetType = ctx.MakeType<TStructExprType>(subsetItems);
        YQL_CLOG(DEBUG, Core) << "FieldSubset for AggApply";
        return ctx.ChangeChild(*node, 1, ExpandType(node->Pos(), *subsetType, ctx));
    };

    map["SessionWindowTraits"] = map["SortTraits"] = map["Lag"] = map["Lead"] = map["RowNumber"] = map["Rank"] = map["DenseRank"] =
        [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx)
    {
        auto structType = node->Child(0)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()
            ->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
        if (node->IsCallable("RowNumber")) {
            if (structType->GetSize() == 0) {
                return node;
            }

            auto subsetType = ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(TVector<const TItemExprType*>()));
            YQL_CLOG(DEBUG, Core) << "FieldSubset for " << node->Content();
            return ctx.Builder(node->Pos())
                .Callable(node->Content())
                    .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                .Seal()
                .Build();
        }

        TSet<ui32> lambdaIndexes;
        TSet<TStringBuf> lambdaSubset;
        if (node->IsCallable("SessionWindowTraits")) {
            lambdaIndexes = { 2, 3, 4 };
            TCoSessionWindowTraits self(node);
            if (auto maybeSort = self.SortSpec().Maybe<TCoSortTraits>()) {
                const TTypeAnnotationNode* itemType =
                    maybeSort.Cast().ListType().Ref().GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TListExprType>()->GetItemType();
                if (itemType->GetKind() == ETypeAnnotationKind::Struct) {
                    for (auto& col : itemType->Cast<TStructExprType>()->GetItems()) {
                        lambdaSubset.insert(col->GetName());
                    }
                }
            }
        } else {
            lambdaIndexes = { node->IsCallable("SortTraits") ? 2u : 1u };
        }

        for (ui32 idx : lambdaIndexes) {
            auto lambda = node->Child(idx);
            if (!HaveFieldsSubset(lambda->ChildPtr(1), *lambda->Child(0)->Child(0), lambdaSubset, *optCtx.ParentsMap)) {
                return node;
            }
        }

        if (lambdaSubset.size() == structType->GetSize()) {
            return node;
        }

        TVector<const TItemExprType*> subsetItems;
        for (const auto& item : structType->GetItems()) {
            if (lambdaSubset.contains(item->GetName())) {
                subsetItems.push_back(item);
            }
        }

        auto subsetType = ctx.MakeType<TListExprType>(ctx.MakeType<TStructExprType>(subsetItems));
        YQL_CLOG(DEBUG, Core) << "FieldSubset for " << node->Content();
        if (node->IsCallable("SortTraits")) {
            return ctx.Builder(node->Pos())
                .Callable("SortTraits")
                    .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                    .Add(1, node->ChildPtr(1))
                    .Add(2, ctx.DeepCopyLambda(*node->ChildPtr(2)))
                .Seal()
                .Build();
        } else if (node->IsCallable("SessionWindowTraits")) {
            return ctx.Builder(node->Pos())
                .Callable("SessionWindowTraits")
                    .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                    .Add(1, node->ChildPtr(1))
                    .Add(2, ctx.DeepCopyLambda(*node->ChildPtr(2)))
                    .Add(3, ctx.DeepCopyLambda(*node->ChildPtr(3)))
                    .Add(4, ctx.DeepCopyLambda(*node->ChildPtr(4)))
                .Seal()
            .Build();
        } else {
            if (node->ChildrenSize() == 2) {
                return ctx.Builder(node->Pos())
                    .Callable(node->Content())
                        .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                        .Add(1, ctx.DeepCopyLambda(*node->ChildPtr(1)))
                    .Seal()
                    .Build();
            } else {
                return ctx.Builder(node->Pos())
                    .Callable(node->Content())
                        .Add(0, ExpandType(node->Pos(), *subsetType, ctx))
                        .Add(1, ctx.DeepCopyLambda(*node->ChildPtr(1)))
                        .Add(2, node->ChildPtr(2))
                    .Seal()
                    .Build();
            }
        }
    };

    map["Aggregate"] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoAggregate self(node);
        if (!optCtx.IsSingleUsage(self.Input()) && !optCtx.IsPersistentNode(self.Input())) {
            return node;
        }

        auto ret = AggregateSubsetFieldsAnalyzer(self, ctx, *optCtx.ParentsMap);
        if (ret != node) {
            YQL_CLOG(DEBUG, Core) << "AggregateSubsetFieldsAnalyzer";
            return ret;
        }

        return node;
    };

    map["CalcOverWindow"] = map["CalcOverSessionWindow"] = map["CalcOverWindowGroup"] =
        [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx)
    {
        if (!optCtx.IsSingleUsage(*node->Child(0))) {
            return node;
        }

        if (!node->Child(0)->IsCallable({"CalcOverWindow", "CalcOverSessionWindow", "CalcOverWindowGroup"})) {
            return node;
        }

        TExprNodeList parentCalcs = ExtractCalcsOverWindow(node, ctx);
        TExprNodeList calcs = ExtractCalcsOverWindow(node->ChildPtr(0), ctx);
        calcs.insert(calcs.end(), parentCalcs.begin(), parentCalcs.end());

        YQL_CLOG(DEBUG, Core) << "Fuse nested CalcOverWindow/CalcOverSessionWindow/CalcOverWindowGroup";

        return RebuildCalcOverWindowGroup(node->Child(0)->Pos(), node->Child(0)->ChildPtr(0), calcs, ctx);
    };

    map[TCoCondense::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoCondense self(node);
        if (!optCtx.IsSingleUsage(self.Input().Ref())) {
            return node;
        }

        std::map<std::string_view, TExprNode::TPtr> usedFields;
        if (HaveFieldsSubset(self.SwitchHandler().Body().Ptr(), self.SwitchHandler().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && HaveFieldsSubset(self.UpdateHandler().Body().Ptr(), self.UpdateHandler().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && usedFields.size() < GetSeqItemType(self.Input().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetSize())
        {
            TExprNode::TListType fields;
            fields.reserve(usedFields.size());
            std::transform(usedFields.begin(), usedFields.end(), std::back_inserter(fields),
                [](std::pair<const std::string_view, TExprNode::TPtr>& item){ return std::move(item.second); });

            YQL_CLOG(DEBUG, Core) << node->Content() << "SubsetFields";
            return Build<TCoCondense>(ctx, node->Pos())
                .Input<TCoExtractMembers>()
                    .Input(self.Input())
                    .Members()
                        .Add(std::move(fields))
                    .Build()
                .Build()
                .SwitchHandler(ctx.DeepCopyLambda(self.SwitchHandler().Ref()))
                .UpdateHandler(ctx.DeepCopyLambda(self.UpdateHandler().Ref()))
                .Done().Ptr();
        }
        return node;
    };

    map[TCoCondense1::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoCondense1 self(node);
        if (!optCtx.IsSingleUsage(self.Input().Ref())) {
            return node;
        }

        std::map<std::string_view, TExprNode::TPtr> usedFields;
        if (HaveFieldsSubset(self.InitHandler().Body().Ptr(), self.InitHandler().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && HaveFieldsSubset(self.SwitchHandler().Body().Ptr(), self.SwitchHandler().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && HaveFieldsSubset(self.UpdateHandler().Body().Ptr(), self.UpdateHandler().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false)
            && !usedFields.empty()
            && usedFields.size() < GetSeqItemType(self.Input().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetSize())
        {
            TExprNode::TListType fields;
            fields.reserve(usedFields.size());
            std::transform(usedFields.begin(), usedFields.end(), std::back_inserter(fields),
                [](std::pair<const std::string_view, TExprNode::TPtr>& item){ return std::move(item.second); });

            YQL_CLOG(DEBUG, Core) << node->Content() << "SubsetFields";
            return Build<TCoCondense1>(ctx, node->Pos())
                .Input<TCoExtractMembers>()
                    .Input(self.Input())
                    .Members()
                        .Add(std::move(fields))
                    .Build()
                .Build()
                .InitHandler(ctx.DeepCopyLambda(self.InitHandler().Ref()))
                .SwitchHandler(ctx.DeepCopyLambda(self.SwitchHandler().Ref()))
                .UpdateHandler(ctx.DeepCopyLambda(self.UpdateHandler().Ref()))
                .Done().Ptr();
        }
        return node;
    };

    map[TCoMapNext::CallableName()] = [](const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
        TCoMapNext self(node);
        if (!optCtx.IsSingleUsage(self.Input().Ref())) {
            return node;
        }

        std::map<std::string_view, TExprNode::TPtr> usedFields;
        if ((
             HaveFieldsSubset(self.Lambda().Body().Ptr(), self.Lambda().Args().Arg(0).Ref(), usedFields, *optCtx.ParentsMap, false) &&
             HaveFieldsSubset(self.Lambda().Body().Ptr(), self.Lambda().Args().Arg(1).Ref(), usedFields, *optCtx.ParentsMap, false)
            ) && usedFields.size() < GetSeqItemType(self.Input().Ref().GetTypeAnn())->Cast<TStructExprType>()->GetSize())
        {
            TExprNode::TListType fields;
            fields.reserve(usedFields.size());
            std::transform(usedFields.begin(), usedFields.end(), std::back_inserter(fields),
                [](std::pair<const std::string_view, TExprNode::TPtr>& item){ return std::move(item.second); });

            YQL_CLOG(DEBUG, Core) << node->Content() << "SubsetFields";
            return Build<TCoMapNext>(ctx, node->Pos())
                .Input<TCoExtractMembers>()
                    .Input(self.Input())
                    .Members()
                        .Add(std::move(fields))
                    .Build()
                .Build()
                .Lambda(ctx.DeepCopyLambda(self.Lambda().Ref()))
                .Done().Ptr();
        }
        return node;
    };
}

}
