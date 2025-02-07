//------------------------------------------------------------------------------
// SelectExpressions.cpp
// Definitions for selection expressions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/binding/SelectExpressions.h"

#include "slang/binding/CallExpression.h"
#include "slang/binding/LiteralExpressions.h"
#include "slang/binding/MiscExpressions.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/ConstEvalDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/symbols/BlockSymbols.h"
#include "slang/symbols/ClassSymbols.h"
#include "slang/symbols/CoverSymbols.h"
#include "slang/symbols/InstanceSymbols.h"
#include "slang/symbols/MemberSymbols.h"
#include "slang/symbols/ParameterSymbols.h"
#include "slang/symbols/SubroutineSymbols.h"
#include "slang/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/types/AllTypes.h"
#include "slang/types/NetType.h"

namespace slang {

static const Type& getIndexedType(Compilation& compilation, const BindContext& context,
                                  const Type& valueType, SourceRange exprRange,
                                  SourceRange valueRange, bool isRangeSelect) {
    const Type& ct = valueType.getCanonicalType();
    if (ct.isArray()) {
        return *ct.getArrayElementType();
    }
    else if (ct.kind == SymbolKind::StringType && !isRangeSelect) {
        return compilation.getByteType();
    }
    else if (!ct.isIntegral()) {
        auto& diag = context.addDiag(diag::BadIndexExpression, exprRange);
        diag << valueRange;
        diag << valueType;
        return compilation.getErrorType();
    }
    else if (ct.isScalar()) {
        auto& diag = context.addDiag(diag::CannotIndexScalar, exprRange);
        diag << valueRange;
        return compilation.getErrorType();
    }
    else if (ct.isFourState()) {
        return compilation.getLogicType();
    }
    else {
        return compilation.getBitType();
    }
}

static void checkForVectoredSelect(const Expression& value, SourceRange range,
                                   const BindContext& context) {
    if (value.kind != ExpressionKind::NamedValue && value.kind != ExpressionKind::HierarchicalValue)
        return;

    const Symbol& sym = value.as<ValueExpressionBase>().symbol;
    if (sym.kind == SymbolKind::Net && sym.as<NetSymbol>().expansionHint == NetSymbol::Vectored) {
        auto& diag = context.addDiag(diag::SelectOfVectoredNet, range);
        diag.addNote(diag::NoteDeclarationHere, sym.location);
    }
}

template<typename T>
bool requireLValueHelper(const T& expr, const BindContext& context, SourceLocation location,
                         bitmask<AssignFlags> flags, const Expression* longestStaticPrefix,
                         EvalContext* customEvalContext) {
    auto& val = expr.value();
    if (val.kind == ExpressionKind::Concatenation || val.kind == ExpressionKind::Streaming) {
        // Selects of concatenations are not allowed to be lvalues.
        if (!location)
            location = expr.sourceRange.start();

        auto& diag = context.addDiag(diag::ExpressionNotAssignable, location);
        diag << expr.sourceRange;
        return false;
    }

    if (ValueExpressionBase::isKind(val.kind)) {
        auto sym = val.getSymbolReference();
        if (sym && sym->kind == SymbolKind::Net) {
            auto& net = sym->template as<NetSymbol>();
            if (net.netType.netKind == NetType::UserDefined) {
                context.addDiag(diag::UserDefPartialDriver, expr.sourceRange) << net.name;
                return false;
            }
        }
    }

    if (context.flags.has(BindFlags::NonProcedural)) {
        if constexpr (std::is_same_v<T, RangeSelectExpression>) {
            if (!context.eval(expr.left()) || !context.eval(expr.right()))
                return false;
        }
        else {
            if (!context.eval(expr.selector()))
                return false;
        }

        if (!longestStaticPrefix)
            longestStaticPrefix = &expr;
    }
    else {
        EvalContext localEvalCtx(context.getCompilation(), EvalFlags::CacheResults);
        EvalContext* evalCtxPtr = customEvalContext;
        if (!evalCtxPtr)
            evalCtxPtr = &localEvalCtx;

        if (expr.isConstantSelect(*evalCtxPtr)) {
            if (!longestStaticPrefix)
                longestStaticPrefix = &expr;
        }
        else {
            longestStaticPrefix = nullptr;
        }
    }

    return val.requireLValue(context, location, flags, longestStaticPrefix, customEvalContext);
}

Expression& ElementSelectExpression::fromSyntax(Compilation& compilation, Expression& value,
                                                const ExpressionSyntax& syntax,
                                                SourceRange fullRange, const BindContext& context) {
    if (value.bad())
        return badExpr(compilation, nullptr);

    // Selects of vectored nets are disallowed.
    checkForVectoredSelect(value, fullRange, context);

    const Type& valueType = *value.type;
    const Type& resultType = getIndexedType(compilation, context, valueType, syntax.sourceRange(),
                                            value.sourceRange, false);

    // If this is an associative array with a specific index target, we need to bind
    // as an rvalue to get the right conversion applied.
    const Expression* selector = nullptr;
    if (valueType.isAssociativeArray()) {
        auto indexType = valueType.getAssociativeIndexType();
        if (indexType)
            selector = &bindRValue(*indexType, syntax, syntax.getFirstToken().location(), context);
    }

    if (!selector) {
        bitmask<BindFlags> flags;
        if (valueType.isQueue())
            flags = BindFlags::AllowUnboundedLiteral | BindFlags::AllowUnboundedLiteralArithmetic;

        selector = &selfDetermined(compilation, syntax, context, flags);
        if (!selector->type->isUnbounded() && !context.requireIntegral(*selector))
            return badExpr(compilation, nullptr);
    }

    auto result =
        compilation.emplace<ElementSelectExpression>(resultType, value, *selector, fullRange);
    if (selector->bad() || result->bad())
        return badExpr(compilation, result);

    // If the selector is constant, and the underlying type has a fixed range,
    // we can do checking at compile time that it's within bounds.
    // Only do that if we're not in an unevaluated conditional branch.
    if (valueType.hasFixedRange()) {
        ConstantValue selVal;
        if (!context.inUnevaluatedBranch() && (selVal = context.tryEval(*selector))) {
            optional<int32_t> index = selVal.integer().as<int32_t>();
            if (!index || !valueType.getFixedRange().containsPoint(*index)) {
                auto& diag = context.addDiag(diag::IndexValueInvalid, selector->sourceRange);
                diag << selVal;
                diag << *value.type;
                return badExpr(compilation, result);
            }
        }
    }
    else if (context.flags.has(BindFlags::NonProcedural)) {
        context.addDiag(diag::DynamicNotProcedural, fullRange);
        return badExpr(compilation, result);
    }

    return *result;
}

Expression& ElementSelectExpression::fromConstant(Compilation& compilation, Expression& value,
                                                  int32_t index, const BindContext& context) {
    Expression* indexExpr = &IntegerLiteral::fromConstant(compilation, index);
    selfDetermined(context, indexExpr);

    const Type& resultType = getIndexedType(compilation, context, *value.type,
                                            indexExpr->sourceRange, value.sourceRange, false);

    auto result = compilation.emplace<ElementSelectExpression>(resultType, value, *indexExpr,
                                                               value.sourceRange);
    if (value.bad() || indexExpr->bad() || result->bad())
        return badExpr(compilation, result);

    return *result;
}

bool ElementSelectExpression::isConstantSelect(EvalContext& context) const {
    return value().type->hasFixedRange() && !selector().eval(context).bad();
}

ConstantValue ElementSelectExpression::evalImpl(EvalContext& context) const {
    ConstantValue cv = value().eval(context);
    if (!cv)
        return nullptr;

    ConstantValue associativeIndex;
    auto range = evalIndex(context, cv, associativeIndex);
    if (!range && associativeIndex.bad())
        return nullptr;

    // Handling for packed and unpacked arrays, all integer types.
    const Type& valType = *value().type;
    if (valType.hasFixedRange()) {
        // For fixed types, we know we will always be in range, so just do the selection.
        if (valType.isUnpackedArray())
            return cv.elements()[size_t(range->left)];
        else
            return cv.integer().slice(range->left, range->right);
    }

    // Handling for associative arrays.
    if (valType.isAssociativeArray()) {
        auto& map = *cv.map();
        if (auto it = map.find(associativeIndex); it != map.end())
            return it->second;

        // If there is a user specified default, return that without warning.
        if (map.defaultValue)
            return map.defaultValue;

        // Otherwise issue a warning and use the default default.
        context.addDiag(diag::ConstEvalAssociativeElementNotFound, selector().sourceRange)
            << value().sourceRange << associativeIndex;
        return type->getDefaultValue();
    }

    // Handling for strings, dynamic arrays, and queues.
    ASSERT(range->left == range->right);
    if (valType.isString())
        return cv.getSlice(range->left, range->right, nullptr);

    // -1 is returned for dynamic array indices that are out of bounds.
    if (range->left == -1)
        return type->getDefaultValue();

    return std::move(cv).at(size_t(range->left));
}

LValue ElementSelectExpression::evalLValueImpl(EvalContext& context) const {
    LValue lval = value().evalLValue(context);
    if (!lval)
        return nullptr;

    ConstantValue loadedVal;
    if (!value().type->hasFixedRange())
        loadedVal = lval.load();

    ConstantValue associativeIndex;
    auto range = evalIndex(context, loadedVal, associativeIndex);
    if (!range && associativeIndex.bad())
        return nullptr;

    // Handling for packed and unpacked arrays, all integer types.
    const Type& valType = *value().type;
    if (valType.hasFixedRange()) {
        // For fixed types, we know we will always be in range, so just do the selection.
        if (valType.isUnpackedArray())
            lval.addIndex(range->left, type->getDefaultValue());
        else
            lval.addBitSlice(*range);
        return lval;
    }

    // Handling for associative arrays.
    if (valType.isAssociativeArray()) {
        lval.addArrayLookup(std::move(associativeIndex), type->getDefaultValue());
        return lval;
    }

    // Handling for strings, dynamic arrays, and queues.
    ASSERT(range->left == range->right);
    if (valType.isString()) {
        lval.addIndex(range->left, nullptr);
    }
    else {
        // -1 is returned for dynamic array indices that are out of bounds.
        // LValue handles selecting elements out of bounds and ignores accesses to those locations.
        lval.addIndex(range->left, type->getDefaultValue());
    }
    return lval;
}

optional<ConstantRange> ElementSelectExpression::evalIndex(EvalContext& context,
                                                           const ConstantValue& val,
                                                           ConstantValue& associativeIndex) const {
    auto prevQ = context.getQueueTarget();
    if (val.isQueue())
        context.setQueueTarget(&val);

    ConstantValue cs = selector().eval(context);

    context.setQueueTarget(prevQ);
    if (!cs)
        return std::nullopt;

    const Type& valType = *value().type;
    if (valType.hasFixedRange()) {
        optional<int32_t> index = cs.integer().as<int32_t>();
        ConstantRange range = valType.getFixedRange();
        if (!index || !range.containsPoint(*index)) {
            context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cs << valType;
            return std::nullopt;
        }

        if (valType.isUnpackedArray()) {
            // Unpacked arrays are stored reversed in memory, so reverse the range here.
            range = range.reverse();
            int32_t i = range.translateIndex(*index);
            return ConstantRange{ i, i };
        }

        // For packed arrays, we're selecting bit ranges, not necessarily single bits,
        // so multiply out by the width of each element.
        int32_t width = (int32_t)type->getBitWidth();
        int32_t i = range.translateIndex(*index) * width;
        return ConstantRange{ i + width - 1, i };
    }

    if (valType.isAssociativeArray()) {
        if (cs.hasUnknown())
            context.addDiag(diag::ConstEvalAssociativeIndexInvalid, selector().sourceRange) << cs;
        else
            associativeIndex = std::move(cs);
        return std::nullopt;
    }

    // TODO: rework errors / warnings for out-of-bounds accesses
    optional<int32_t> index = cs.integer().as<int32_t>();
    if (!index) {
        context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cs << valType;
        return std::nullopt;
    }

    if (!val)
        return ConstantRange{ *index, *index };

    if (valType.isString()) {
        const std::string& str = val.str();
        if (!index || *index < 0 || size_t(*index) >= str.size()) {
            context.addDiag(diag::ConstEvalStringIndexInvalid, sourceRange) << cs << str.size();
            return std::nullopt;
        }

        return ConstantRange{ *index, *index };
    }

    size_t maxIndex = val.size();
    if (val.isQueue())
        maxIndex++;

    if (!index || *index < 0 || size_t(*index) >= maxIndex) {
        context.addDiag(diag::ConstEvalDynamicArrayIndex, sourceRange) << cs << valType << maxIndex;

        // Return a sentinel value (which is never valid as a dynamic array index).
        return ConstantRange{ -1, -1 };
    }

    return ConstantRange{ *index, *index };
}

bool ElementSelectExpression::requireLValueImpl(const BindContext& context, SourceLocation location,
                                                bitmask<AssignFlags> flags,
                                                const Expression* longestStaticPrefix,
                                                EvalContext* customEvalContext) const {
    return requireLValueHelper(*this, context, location, flags, longestStaticPrefix,
                               customEvalContext);
}

void ElementSelectExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("value", value());
    serializer.write("selector", selector());
}

Expression& RangeSelectExpression::fromSyntax(Compilation& compilation, Expression& value,
                                              const RangeSelectSyntax& syntax,
                                              SourceRange fullRange, const BindContext& context) {
    // Left and right are either the extents of a part-select, in which case they must
    // both be constant, or the left hand side is the start and the right hand side is
    // the width of an indexed part select, in which case only the rhs need be constant.
    RangeSelectionKind selectionKind;
    switch (syntax.kind) {
        case SyntaxKind::SimpleRangeSelect:
            selectionKind = RangeSelectionKind::Simple;
            break;
        case SyntaxKind::AscendingRangeSelect:
            selectionKind = RangeSelectionKind::IndexedUp;
            break;
        case SyntaxKind::DescendingRangeSelect:
            selectionKind = RangeSelectionKind::IndexedDown;
            break;
        default:
            THROW_UNREACHABLE;
    }

    if (!value.bad() && value.type->isAssociativeArray()) {
        context.addDiag(diag::RangeSelectAssociative, fullRange);
        return badExpr(compilation, nullptr);
    }

    // Selection expressions don't need to be const if we're selecting from a queue.
    bitmask<BindFlags> extraFlags;
    bool isQueue = value.type->isQueue();
    if (isQueue)
        extraFlags = BindFlags::AllowUnboundedLiteral | BindFlags::AllowUnboundedLiteralArithmetic;

    auto& left = bind(*syntax.left, context, extraFlags);
    auto& right = bind(*syntax.right, context, extraFlags);

    auto result = compilation.emplace<RangeSelectExpression>(
        selectionKind, compilation.getErrorType(), value, left, right, fullRange);

    if (value.bad() || left.bad() || right.bad())
        return badExpr(compilation, result);

    if (!left.type->isUnbounded() && !context.requireIntegral(left))
        return badExpr(compilation, result);

    if (!right.type->isUnbounded() && !context.requireIntegral(right))
        return badExpr(compilation, result);

    const Type& valueType = *value.type;
    const Type& elementType = getIndexedType(compilation, context, valueType, syntax.sourceRange(),
                                             value.sourceRange, true);
    if (elementType.isError())
        return badExpr(compilation, result);

    // Selects of vectored nets are disallowed.
    checkForVectoredSelect(value, fullRange, context);

    if (!valueType.hasFixedRange() && context.flags.has(BindFlags::NonProcedural)) {
        context.addDiag(diag::DynamicNotProcedural, fullRange);
        return badExpr(compilation, result);
    }

    // If this is selecting from a queue, the result is always a queue.
    if (isQueue) {
        result->type = compilation.emplace<QueueType>(elementType, 0u);
        return *result;
    }

    // If not a queue, rhs must always be a constant integer.
    optional<int32_t> rv = context.evalInteger(right);
    if (!rv)
        return badExpr(compilation, result);

    // If the array type has a fixed range, validate that the range we're selecting is allowed.
    SourceRange errorRange{ left.sourceRange.start(), right.sourceRange.end() };
    if (valueType.hasFixedRange()) {
        ConstantRange selectionRange;
        ConstantRange valueRange = valueType.getFixedRange();

        // Helper function for validating the bounds of the selection.
        auto validateRange = [&](auto range) {
            if (!valueRange.containsPoint(range.left) || !valueRange.containsPoint(range.right)) {
                auto& diag = context.addDiag(diag::BadRangeExpression, errorRange);
                diag << range.left << range.right;
                diag << valueType;
                return false;
            }
            return true;
        };

        if (selectionKind == RangeSelectionKind::Simple) {
            optional<int32_t> lv = context.evalInteger(left);
            if (!lv)
                return badExpr(compilation, result);

            selectionRange = { *lv, *rv };
            if (selectionRange.isLittleEndian() != valueRange.isLittleEndian() &&
                selectionRange.width() > 1) {
                auto& diag = context.addDiag(diag::SelectEndianMismatch, errorRange);
                diag << valueType;
                return badExpr(compilation, result);
            }

            if (!context.inUnevaluatedBranch() && !validateRange(selectionRange))
                return badExpr(compilation, result);
        }
        else {
            if (!context.requireGtZero(rv, right.sourceRange))
                return badExpr(compilation, result);

            if (bitwidth_t(*rv) > valueRange.width()) {
                auto& diag = context.addDiag(diag::RangeWidthTooLarge, right.sourceRange);
                diag << *rv;
                diag << valueType;
                return badExpr(compilation, result);
            }

            // If the lhs is a known constant, we can check that now too.
            ConstantValue leftVal;
            if (!context.inUnevaluatedBranch() && (leftVal = context.tryEval(left))) {
                optional<int32_t> index = leftVal.integer().as<int32_t>();
                if (!index) {
                    auto& diag = context.addDiag(diag::IndexValueInvalid, left.sourceRange);
                    diag << leftVal;
                    diag << valueType;
                    return badExpr(compilation, result);
                }

                selectionRange =
                    ConstantRange::getIndexedRange(*index, *rv, valueRange.isLittleEndian(),
                                                   selectionKind == RangeSelectionKind::IndexedUp);

                if (!validateRange(selectionRange))
                    return badExpr(compilation, result);
            }
            else {
                // Otherwise, the resulting range will start with the fixed lower bound of the type.
                int32_t l = selectionKind == RangeSelectionKind::IndexedUp ? valueRange.lower()
                                                                           : valueRange.upper();
                selectionRange =
                    ConstantRange::getIndexedRange(l, *rv, valueRange.isLittleEndian(),
                                                   selectionKind == RangeSelectionKind::IndexedUp);
            }
        }

        // At this point, all expressions are good, ranges have been validated and
        // we know the final width of the selection, so pick the result type and we're done.
        if (valueType.isUnpackedArray()) {
            result->type =
                compilation.emplace<FixedSizeUnpackedArrayType>(elementType, selectionRange);
        }
        else {
            result->type = compilation.emplace<PackedArrayType>(elementType, selectionRange);
        }
    }
    else {
        // Otherwise, this is a dynamic array so we can't validate much. We should check that
        // the selection endianness is correct for simple ranges -- dynamic arrays only
        // permit big endian [0..N] ordering.
        ConstantRange selectionRange;
        if (selectionKind == RangeSelectionKind::Simple) {
            optional<int32_t> lv = context.evalInteger(left);
            if (!lv)
                return badExpr(compilation, result);

            selectionRange = { *lv, *rv };
            if (selectionRange.isLittleEndian() && selectionRange.width() > 1) {
                auto& diag = context.addDiag(diag::SelectEndianDynamic, errorRange);
                diag << selectionRange.left << selectionRange.right;
                diag << valueType;
                return badExpr(compilation, result);
            }
        }
        else {
            if (!context.requireGtZero(rv, right.sourceRange))
                return badExpr(compilation, result);

            selectionRange.left = 0;
            selectionRange.right = *rv - 1;
        }

        result->type = compilation.emplace<FixedSizeUnpackedArrayType>(elementType, selectionRange);
    }

    return *result;
}

Expression& RangeSelectExpression::fromConstant(Compilation& compilation, Expression& value,
                                                ConstantRange range, const BindContext& context) {
    Expression* left = &IntegerLiteral::fromConstant(compilation, range.left);
    selfDetermined(context, left);

    Expression* right = &IntegerLiteral::fromConstant(compilation, range.right);
    selfDetermined(context, right);

    auto result = compilation.emplace<RangeSelectExpression>(RangeSelectionKind::Simple,
                                                             compilation.getErrorType(), value,
                                                             *left, *right, value.sourceRange);
    if (value.bad() || left->bad() || right->bad())
        return badExpr(compilation, result);

    const Type& valueType = *value.type;
    const Type& elementType =
        getIndexedType(compilation, context, valueType, value.sourceRange, value.sourceRange, true);

    if (elementType.isError())
        return badExpr(compilation, result);

    // This method is only called on expressions with a fixed range type.
    ConstantRange valueRange = valueType.getFixedRange();
    ASSERT(range.isLittleEndian() == valueRange.isLittleEndian());
    ASSERT(valueType.hasFixedRange());

    if (valueType.isUnpackedArray())
        result->type = compilation.emplace<FixedSizeUnpackedArrayType>(elementType, range);
    else
        result->type = compilation.emplace<PackedArrayType>(elementType, range);

    return *result;
}

bool RangeSelectExpression::isConstantSelect(EvalContext& context) const {
    return value().type->hasFixedRange() && left().eval(context) && right().eval(context);
}

ConstantValue RangeSelectExpression::evalImpl(EvalContext& context) const {
    ConstantValue cv = value().eval(context);
    if (!cv)
        return nullptr;

    auto range = evalRange(context, cv);
    if (!range)
        return nullptr;

    if (value().type->hasFixedRange())
        return cv.getSlice(range->upper(), range->lower(), nullptr);

    // If this is a queue, we didn't verify the endianness of the selection.
    // Check if it's reversed here and issue a warning if so.
    if (value().type->isQueue() && range->isLittleEndian() && range->left != range->right) {
        context.addDiag(diag::ConstEvalQueueRange, sourceRange) << range->left << range->right;
        return value().type->getDefaultValue();
    }

    return cv.getSlice(range->upper(), range->lower(),
                       type->getArrayElementType()->getDefaultValue());
}

LValue RangeSelectExpression::evalLValueImpl(EvalContext& context) const {
    LValue lval = value().evalLValue(context);
    if (!lval)
        return nullptr;

    ConstantValue loadedVal;
    if (!value().type->hasFixedRange())
        loadedVal = lval.load();

    auto range = evalRange(context, loadedVal);
    if (!range)
        return nullptr;

    if (value().type->hasFixedRange()) {
        if (value().type->isIntegral())
            lval.addBitSlice(*range);
        else
            lval.addArraySlice(*range, nullptr);
    }
    else {
        lval.addArraySlice(*range, type->getArrayElementType()->getDefaultValue());
    }

    return lval;
}

optional<ConstantRange> RangeSelectExpression::evalRange(EvalContext& context,
                                                         const ConstantValue& val) const {
    auto prevQ = context.getQueueTarget();
    if (val.isQueue())
        context.setQueueTarget(&val);

    ConstantValue cl = left().eval(context);
    ConstantValue cr = right().eval(context);

    context.setQueueTarget(prevQ);
    if (!cl || !cr)
        return std::nullopt;

    const Type& valueType = *value().type;
    if (valueType.hasFixedRange()) {
        ConstantRange result;
        ConstantRange valueRange = valueType.getFixedRange();

        if (selectionKind == RangeSelectionKind::Simple) {
            result = type->getFixedRange();
        }
        else {
            optional<int32_t> l = cl.integer().as<int32_t>();
            if (!l) {
                context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cl << valueType;
                return std::nullopt;
            }

            optional<int32_t> r = cr.integer().as<int32_t>();
            ASSERT(r);
            result = ConstantRange::getIndexedRange(*l, *r, valueRange.isLittleEndian(),
                                                    selectionKind == RangeSelectionKind::IndexedUp);
        }

        if (!valueRange.containsPoint(result.left) || !valueRange.containsPoint(result.right)) {
            auto& diag = context.addDiag(diag::ConstEvalPartSelectInvalid, sourceRange);
            diag << result.left << result.right;
            diag << valueType;
            return std::nullopt;
        }

        if (!valueType.isPackedArray()) {
            if (valueType.isUnpackedArray()) {
                // Unpacked arrays are stored reversed in memory, so reverse the range here.
                valueRange = valueRange.reverse();
            }
            result.left = valueRange.translateIndex(result.left);
            result.right = valueRange.translateIndex(result.right);
            return result;
        }

        // For packed arrays we're potentially selecting multi-bit elements.
        int32_t width = (int32_t)valueType.getArrayElementType()->getBitWidth();
        result.left = valueRange.translateIndex(result.left) * width + width - 1;
        result.right = valueRange.translateIndex(result.right) * width;

        return result;
    }

    optional<int32_t> li = cl.integer().as<int32_t>();
    optional<int32_t> ri = cr.integer().as<int32_t>();
    if (!li) {
        context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cl << valueType;
        return std::nullopt;
    }
    if (!ri) {
        context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cr << valueType;
        return std::nullopt;
    }

    int32_t l = *li;
    int32_t r = *ri;
    ConstantRange result;

    if (selectionKind == RangeSelectionKind::Simple) {
        result = { l, r };
    }
    else {
        result = ConstantRange::getIndexedRange(l, r, false,
                                                selectionKind == RangeSelectionKind::IndexedUp);
    }

    // Out of bounds ranges are allowed, we just issue a warning.
    if (!val.bad()) {
        size_t size = val.size();
        if (l < 0 || r < 0 || size_t(r) >= size) {
            auto& diag = context.addDiag(diag::ConstEvalDynamicArrayRange, sourceRange);
            diag << result.left << result.right;
            diag << valueType;
            diag << size;
        }
    }

    // TODO: warn on negative indices when we don't have a value to check the size against

    return result;
}

bool RangeSelectExpression::requireLValueImpl(const BindContext& context, SourceLocation location,
                                              bitmask<AssignFlags> flags,
                                              const Expression* longestStaticPrefix,
                                              EvalContext* customEvalContext) const {
    return requireLValueHelper(*this, context, location, flags, longestStaticPrefix,
                               customEvalContext);
}

void RangeSelectExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("selectionKind", toString(selectionKind));
    serializer.write("value", value());
    serializer.write("left", left());
    serializer.write("right", right());
}

static Expression* tryBindSpecialMethod(Compilation& compilation, const Expression& expr,
                                        const LookupResult::MemberSelector& selector,
                                        const InvocationExpressionSyntax* invocation,
                                        const ArrayOrRandomizeMethodExpressionSyntax* withClause,
                                        const BindContext& context) {
    auto sym = expr.getSymbolReference();
    if (!sym)
        return nullptr;

    // There is a built-in 'rand_mode' method that is present on every 'rand' and 'randc'
    // class property, and additionally on subelements of those properties.
    if (selector.name == "rand_mode"sv) {
        if (sym->getRandMode() == RandMode::None)
            return nullptr;

        return CallExpression::fromBuiltInMethod(compilation, SymbolKind::ClassProperty, expr,
                                                 selector, invocation, withClause, context);
    }

    return CallExpression::fromBuiltInMethod(compilation, sym->kind, expr, selector, invocation,
                                             withClause, context);
}

Expression& MemberAccessExpression::fromSelector(
    Compilation& compilation, Expression& expr, const LookupResult::MemberSelector& selector,
    const InvocationExpressionSyntax* invocation,
    const ArrayOrRandomizeMethodExpressionSyntax* withClause, const BindContext& context) {

    // If the selector name is invalid just give up early.
    if (selector.name.empty())
        return badExpr(compilation, &expr);

    // The source range of the entire member access starts from the value being selected.
    SourceRange range{ expr.sourceRange.start(), selector.nameRange.end() };

    // Special cases for built-in iterator methods that don't cleanly fit the general
    // mold of looking up members via the type of the expression.
    if (expr.kind == ExpressionKind::NamedValue) {
        auto symKind = expr.as<NamedValueExpression>().symbol.kind;
        if (symKind == SymbolKind::Iterator) {
            auto result = CallExpression::fromBuiltInMethod(compilation, symKind, expr, selector,
                                                            invocation, withClause, context);
            if (result)
                return *result;
        }
    }

    auto errorIfNotProcedural = [&] {
        if (context.flags.has(BindFlags::NonProcedural)) {
            context.addDiag(diag::DynamicNotProcedural, range);
            return true;
        }
        return false;
    };
    auto errorIfAssertion = [&] {
        if (context.flags.has(BindFlags::AssertionExpr)) {
            context.addDiag(diag::ClassMemberInAssertion, range);
            return true;
        }
        return false;
    };

    // This might look like a member access but actually be a built-in type method.
    const Type& type = expr.type->getCanonicalType();
    const Scope* scope = nullptr;
    switch (type.kind) {
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::PackedUnionType:
        case SymbolKind::UnpackedUnionType:
            scope = &type.as<Scope>();
            break;
        case SymbolKind::ClassType: {
            auto& ct = type.as<ClassType>();
            if (auto base = ct.getBaseClass(); base && base->isError())
                return badExpr(compilation, &expr);

            scope = &ct;
            break;
        }
        case SymbolKind::CovergroupType:
            scope = &type.as<CovergroupType>().body;
            break;
        case SymbolKind::EnumType:
        case SymbolKind::StringType:
        case SymbolKind::FixedSizeUnpackedArrayType:
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
        case SymbolKind::EventType:
        case SymbolKind::SequenceType: {
            if (auto result = tryBindSpecialMethod(compilation, expr, selector, invocation,
                                                   withClause, context)) {
                return *result;
            }

            return CallExpression::fromSystemMethod(compilation, expr, selector, invocation,
                                                    withClause, context);
        }
        case SymbolKind::VoidType:
            if (auto sym = expr.getSymbolReference()) {
                if (sym->kind == SymbolKind::Coverpoint) {
                    scope = &sym->as<CoverpointSymbol>();
                    break;
                }
                else if (sym->kind == SymbolKind::CoverCross) {
                    scope = &sym->as<CoverCrossSymbol>();
                    break;
                }
            }
            [[fallthrough]];
        default: {
            if (auto result = tryBindSpecialMethod(compilation, expr, selector, invocation,
                                                   withClause, context)) {
                return *result;
            }

            auto& diag = context.addDiag(diag::InvalidMemberAccess, selector.dotLocation);
            diag << expr.sourceRange;
            diag << selector.nameRange;
            diag << *expr.type;
            return badExpr(compilation, &expr);
        }
    }

    const Symbol* member = scope->find(selector.name);
    if (!member) {
        if (auto result = tryBindSpecialMethod(compilation, expr, selector, invocation, withClause,
                                               context)) {
            return *result;
        }

        auto& diag = context.addDiag(diag::UnknownMember, selector.nameRange.start());
        diag << expr.sourceRange;
        diag << selector.name;
        diag << *expr.type;
        return badExpr(compilation, &expr);
    }

    switch (member->kind) {
        case SymbolKind::Field: {
            auto& field = member->as<FieldSymbol>();
            return *compilation.emplace<MemberAccessExpression>(field.getType(), expr, field,
                                                                field.offset, range);
        }
        case SymbolKind::ClassProperty: {
            Lookup::ensureVisible(*member, context, selector.nameRange);
            auto& prop = member->as<ClassPropertySymbol>();
            if (prop.lifetime == VariableLifetime::Automatic &&
                (errorIfNotProcedural() || errorIfAssertion())) {
                return badExpr(compilation, &expr);
            }

            return *compilation.emplace<MemberAccessExpression>(prop.getType(), expr, prop, 0u,
                                                                range);
        }
        case SymbolKind::Subroutine: {
            Lookup::ensureVisible(*member, context, selector.nameRange);
            auto& sub = member->as<SubroutineSymbol>();
            if (!sub.flags.has(MethodFlags::Static) &&
                (errorIfNotProcedural() || errorIfAssertion())) {
                return badExpr(compilation, &expr);
            }

            return CallExpression::fromLookup(compilation, &sub, &expr, invocation, withClause,
                                              range, context);
        }
        case SymbolKind::ConstraintBlock:
        case SymbolKind::Coverpoint:
        case SymbolKind::CoverCross:
        case SymbolKind::CoverageBin: {
            if (errorIfNotProcedural())
                return badExpr(compilation, &expr);
            return *compilation.emplace<MemberAccessExpression>(compilation.getVoidType(), expr,
                                                                *member, 0u, range);
        }
        case SymbolKind::EnumValue:
            // The thing being selected from doesn't actually matter, since the
            // enum value is a constant.
            return ValueExpressionBase::fromSymbol(context, *member, /* isHierarchical */ false,
                                                   range);
        default: {
            if (member->isValue()) {
                auto& value = member->as<ValueSymbol>();
                return *compilation.emplace<MemberAccessExpression>(value.getType(), expr, value,
                                                                    0u, range);
            }

            auto& diag = context.addDiag(diag::InvalidClassAccess, selector.dotLocation);
            diag << selector.nameRange;
            diag << expr.sourceRange;
            diag << selector.name;
            diag << *expr.type;
            return badExpr(compilation, &expr);
        }
    }
}

Expression& MemberAccessExpression::fromSyntax(
    Compilation& compilation, const MemberAccessExpressionSyntax& syntax,
    const InvocationExpressionSyntax* invocation,
    const ArrayOrRandomizeMethodExpressionSyntax* withClause, const BindContext& context) {

    auto name = syntax.name.valueText();
    Expression& lhs = selfDetermined(compilation, *syntax.left, context);
    if (lhs.bad() || name.empty())
        return badExpr(compilation, &lhs);

    LookupResult::MemberSelector selector;
    selector.name = name;
    selector.dotLocation = syntax.dot.location();
    selector.nameRange = syntax.name.range();

    auto& result = fromSelector(compilation, lhs, selector, invocation, withClause, context);
    if (result.kind != ExpressionKind::Call && !result.bad()) {
        if (invocation) {
            auto& diag = context.addDiag(diag::ExpressionNotCallable, invocation->sourceRange());
            diag << selector.nameRange;
            return badExpr(compilation, &result);
        }

        if (withClause)
            context.addDiag(diag::UnexpectedWithClause, withClause->with.range());
    }

    return result;
}

// This iterator is used when translating values between different union members.
// It walks recursively down through unpacked struct members and allows retrieving
// corresponding constant values in member order, as long as they are equivalent
// with the next expected type.
class RecursiveStructMemberIterator {
public:
    RecursiveStructMemberIterator(const ConstantValue& startVal, const Type& startType) {
        curr.val = &startVal;
        curr.type = &startType;

        if (curr.type->isUnpackedStruct()) {
            auto range =
                curr.type->getCanonicalType().as<UnpackedStructType>().membersOfType<FieldSymbol>();
            curr.fieldIt = range.begin();
            curr.fieldEnd = range.end();
            prepNext();
        }
    }

    const ConstantValue* tryConsume(const Type& targetType) {
        if (!curr.type)
            return nullptr;

        if (!curr.type->isUnpackedStruct()) {
            if (!curr.type->isEquivalent(targetType))
                return nullptr;

            curr.type = nullptr;
            return curr.val;
        }

        if (!curr.fieldIt->getType().isEquivalent(targetType))
            return nullptr;

        auto result = &curr.val->at(curr.valIndex);
        curr.next();
        prepNext();
        return result;
    }

private:
    void prepNext() {
        if (curr.fieldIt == curr.fieldEnd) {
            if (stack.empty()) {
                curr.type = nullptr;
                return;
            }

            curr = stack.back();
            stack.pop();

            curr.next();
            prepNext();
        }
        else {
            auto& type = curr.fieldIt->getType();
            if (type.isUnpackedStruct()) {
                stack.emplace(curr);

                auto range =
                    type.getCanonicalType().as<UnpackedStructType>().membersOfType<FieldSymbol>();
                curr.type = &type;
                curr.val = &curr.val->at(curr.valIndex);
                curr.fieldIt = range.begin();
                curr.fieldEnd = range.end();
                curr.valIndex = 0;
                prepNext();
            }
        }
    }

    using FieldIt = Scope::specific_symbol_iterator<FieldSymbol>;

    struct State {
        const ConstantValue* val = nullptr;
        const Type* type = nullptr;
        size_t valIndex = 0;
        FieldIt fieldIt;
        FieldIt fieldEnd;

        void next() {
            fieldIt++;
            valIndex++;
        }
    };

    State curr;
    SmallVectorSized<State, 4> stack;
};

static bool translateUnionMembers(ConstantValue& result, const Type& targetType,
                                  RecursiveStructMemberIterator& rsmi) {
    // If the target type is still an unpacked struct then recurse deeper until we
    // reach a non-struct member that can be assigned a value.
    if (targetType.isUnpackedStruct()) {
        size_t i = 0;
        for (auto& member : targetType.as<UnpackedStructType>().membersOfType<FieldSymbol>()) {
            if (!translateUnionMembers(result.at(i++), member.getType().getCanonicalType(), rsmi)) {
                return false;
            }
        }
        return true;
    }

    auto val = rsmi.tryConsume(targetType);
    if (val) {
        result = *val;
        return true;
    }

    return false;
}

static bool checkPackedUnionTag(const Type& valueType, const SVInt& val, uint32_t expectedTag,
                                EvalContext& context, SourceRange sourceRange,
                                string_view memberName) {
    uint32_t tagBits = valueType.as<PackedUnionType>().tagBits;
    if (tagBits) {
        bitwidth_t bits = val.getBitWidth();
        auto tag = val.slice(int32_t(bits - 1), int32_t(bits - tagBits)).as<uint32_t>();
        if (tag.value() != expectedTag) {
            context.addDiag(diag::ConstEvalTaggedUnion, sourceRange) << memberName;
            return false;
        }
    }

    return true;
}

ConstantValue MemberAccessExpression::evalImpl(EvalContext& context) const {
    ConstantValue cv = value().eval(context);
    if (!cv)
        return nullptr;

    auto& valueType = value().type->getCanonicalType();
    if (valueType.isUnpackedStruct()) {
        return cv.elements()[offset];
    }
    else if (valueType.isUnpackedUnion()) {
        auto& unionVal = cv.unionVal();
        if (unionVal->activeMember == offset)
            return unionVal->value;

        if (valueType.isTaggedUnion()) {
            // Tagged unions can only be accessed via their active member.
            context.addDiag(diag::ConstEvalTaggedUnion, sourceRange) << member.name;
            return nullptr;
        }
        else {
            // This member isn't active, so in general it's not safe (or even
            // possible) to access it. An exception is made for the common initial
            // sequence of equivalent types, so check for that here and if found
            // translate the values across.
            ConstantValue result = type->getDefaultValue();
            if (unionVal->activeMember) {
                // Get the type of the member that is currently active.
                auto& currType = valueType.as<UnpackedUnionType>()
                                     .memberAt<FieldSymbol>(*unionVal->activeMember)
                                     .getType()
                                     .getCanonicalType();

                RecursiveStructMemberIterator rsmi(unionVal->value, currType);
                translateUnionMembers(result, type->getCanonicalType(), rsmi);
            }
            return result;
        }
    }
    else if (valueType.isPackedUnion()) {
        auto& cvi = cv.integer();
        if (!checkPackedUnionTag(valueType, cvi, offset, context, sourceRange, member.name)) {
            return nullptr;
        }

        return cvi.slice(int32_t(type->getBitWidth() - 1), 0);
    }
    else {
        int32_t io = (int32_t)offset;
        int32_t width = (int32_t)type->getBitWidth();
        return cv.integer().slice(width + io - 1, io);
    }
}

LValue MemberAccessExpression::evalLValueImpl(EvalContext& context) const {
    LValue lval = value().evalLValue(context);
    if (!lval)
        return nullptr;

    int32_t io = (int32_t)offset;
    auto& valueType = value().type->getCanonicalType();
    if (valueType.isUnpackedStruct()) {
        lval.addIndex(io, nullptr);
    }
    else if (valueType.isUnpackedUnion()) {
        if (valueType.isTaggedUnion()) {
            auto target = lval.resolve();
            ASSERT(target);

            if (target->unionVal()->activeMember != offset) {
                context.addDiag(diag::ConstEvalTaggedUnion, sourceRange) << member.name;
                return nullptr;
            }
        }
        lval.addIndex(io, type->getDefaultValue());
    }
    else if (valueType.isPackedUnion()) {
        auto cv = lval.load();
        if (!checkPackedUnionTag(valueType, cv.integer(), offset, context, sourceRange,
                                 member.name)) {
            return nullptr;
        }

        int32_t width = (int32_t)type->getBitWidth();
        lval.addBitSlice({ width - 1, 0 });
    }
    else {
        int32_t width = (int32_t)type->getBitWidth();
        lval.addBitSlice({ width + io - 1, io });
    }

    return lval;
}

ConstantRange MemberAccessExpression::getSelectRange() const {
    int32_t io = (int32_t)offset;
    auto& valueType = value().type->getCanonicalType();
    if (valueType.isUnpackedStruct()) {
        return { io, io };
    }
    else if (valueType.isUnpackedUnion()) {
        return { 0, 0 };
    }
    else if (valueType.isPackedUnion()) {
        int32_t width = (int32_t)type->getBitWidth();
        return { width - 1, 0 };
    }
    else {
        int32_t width = (int32_t)type->getBitWidth();
        return { width + io - 1, io };
    }
}

static bool isWithinCovergroup(const Symbol& field, const Scope& usageScope) {
    const Scope* scope = field.getParentScope();
    while (scope) {
        switch (scope->asSymbol().kind) {
            case SymbolKind::CovergroupType:
            case SymbolKind::CovergroupBody:
            case SymbolKind::Coverpoint:
            case SymbolKind::CoverCross:
                return scope == &usageScope;
            default:
                scope = scope->asSymbol().getParentScope();
                break;
        }
    }
    return false;
}

bool MemberAccessExpression::requireLValueImpl(const BindContext& context, SourceLocation location,
                                               bitmask<AssignFlags> flags,
                                               const Expression* longestStaticPrefix,
                                               EvalContext* customEvalContext) const {
    // If this is a selection of a class member, assignability depends only on the selected
    // member and not on the class handle itself. Otherwise, the opposite is true.
    auto& valueType = *value().type;
    if (!valueType.isClass()) {
        if (VariableSymbol::isKind(member.kind) &&
            member.as<VariableSymbol>().flags.has(VariableFlags::ImmutableCoverageOption) &&
            !isWithinCovergroup(member, *context.scope)) {
            context.addDiag(diag::CoverOptionImmutable, location) << member.name;
            return false;
        }

        if (auto sym = value().getSymbolReference(); sym && sym->kind == SymbolKind::Net) {
            auto& net = sym->as<NetSymbol>();
            if (net.netType.netKind == NetType::UserDefined)
                context.addDiag(diag::UserDefPartialDriver, sourceRange) << net.name;
        }

        if (!longestStaticPrefix)
            longestStaticPrefix = this;

        return value().requireLValue(context, location, flags, longestStaticPrefix,
                                     customEvalContext);
    }

    if (VariableSymbol::isKind(member.kind)) {
        if (!longestStaticPrefix)
            longestStaticPrefix = this;

        auto& var = member.as<VariableSymbol>();
        context.addDriver(var, *longestStaticPrefix, flags, customEvalContext);

        return ValueExpressionBase::checkVariableAssignment(context, var, flags, location,
                                                            sourceRange);
    }

    // TODO: modport assignability checks
    if (member.kind == SymbolKind::ModportPort)
        return true;

    if (!location)
        location = sourceRange.start();

    auto& diag = context.addDiag(diag::ExpressionNotAssignable, location);
    diag.addNote(diag::NoteDeclarationHere, member.location);
    diag << sourceRange;
    return false;
}

void MemberAccessExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("member", member);
    serializer.write("value", value());
}

} // namespace slang
