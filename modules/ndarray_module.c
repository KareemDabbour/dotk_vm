#include "../src/include/native_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDARRAY_MAGIC ((uint64_t)0x4E44415252415931ULL)
#define NDARRAY_MAX_DIMS 8

typedef enum
{
    ND_DTYPE_FLOAT64 = 0,
    ND_DTYPE_INT64 = 1,
} NdDType;

typedef struct
{
    uint64_t magic;
    int dtype;
    int ndim;
    int64_t size;
    int64_t shape[NDARRAY_MAX_DIMS];
    int64_t strides[NDARRAY_MAX_DIMS];
    unsigned char data[];
} DotKNdArray;

static const DotKNativeApi *g_api = NULL;

static bool expect_argc(const char *name, int got, int expected, bool *hasError)
{
    if (got == expected)
        return true;
    g_api->raiseError("%s expects %d argument(s) but got %d", name, expected, got);
    *hasError = true;
    return false;
}

static bool expect_number(Value v, const char *name, int index, bool *hasError)
{
    if (IS_NUM(v))
        return true;
    g_api->raiseError("%s expects argument %d to be Number but got %s", name, index, g_api->valueTypeName(v));
    *hasError = true;
    return false;
}

static bool expect_list(Value v, const char *name, int index, bool *hasError)
{
    if (IS_LIST(v))
        return true;
    g_api->raiseError("%s expects argument %d to be List but got %s", name, index, g_api->valueTypeName(v));
    *hasError = true;
    return false;
}

static size_t dtype_elem_size(int dtype)
{
    (void)dtype;
    return sizeof(int64_t);
}

static bool parse_dtype_from_optional(Value v, int *outDtype, const char *name, int index, bool *hasError)
{
    if (IS_NIL(v))
    {
        *outDtype = ND_DTYPE_FLOAT64;
        return true;
    }

    if (!IS_STR(v))
    {
        g_api->raiseError("%s expects argument %d to be String dtype (float64|int64) but got %s", name, index, g_api->valueTypeName(v));
        *hasError = true;
        return false;
    }

    const char *dtypeStr = AS_CSTR(v);
    if (strcmp(dtypeStr, "float64") == 0)
    {
        *outDtype = ND_DTYPE_FLOAT64;
        return true;
    }
    if (strcmp(dtypeStr, "int64") == 0)
    {
        *outDtype = ND_DTYPE_INT64;
        return true;
    }

    g_api->raiseError("Unsupported dtype '%s'. Expected float64 or int64", dtypeStr);
    *hasError = true;
    return false;
}

static bool parse_shape_list(Value shapeVal, int *outNdim, int64_t outShape[NDARRAY_MAX_DIMS], int64_t *outSize, const char *name, bool *hasError)
{
    if (!IS_LIST(shapeVal))
    {
        g_api->raiseError("%s expects shape to be a List of positive integers", name);
        *hasError = true;
        return false;
    }

    ObjList *shapeList = AS_LIST(shapeVal);
    if (shapeList->count <= 0 || shapeList->count > NDARRAY_MAX_DIMS)
    {
        g_api->raiseError("%s shape rank must be between 1 and %d", name, NDARRAY_MAX_DIMS);
        *hasError = true;
        return false;
    }

    int64_t size = 1;
    for (int i = 0; i < shapeList->count; i++)
    {
        Value dimV = shapeList->items[i];
        if (!IS_NUM(dimV))
        {
            g_api->raiseError("%s shape entries must be numbers", name);
            *hasError = true;
            return false;
        }

        double dimD = AS_NUM(dimV);
        int64_t dim = (int64_t)dimD;
        if (dimD != (double)dim || dim <= 0)
        {
            g_api->raiseError("%s shape entries must be positive integers", name);
            *hasError = true;
            return false;
        }

        if (size > INT64_MAX / dim)
        {
            g_api->raiseError("%s shape is too large", name);
            *hasError = true;
            return false;
        }

        outShape[i] = dim;
        size *= dim;
    }

    *outNdim = shapeList->count;
    *outSize = size;
    return true;
}

static DotKNdArray *ndarray_alloc(int dtype, int ndim, const int64_t *shape, int64_t size, bool *hasError)
{
    size_t elemSize = dtype_elem_size(dtype);
    size_t dataBytes = (size_t)size * elemSize;
    size_t totalBytes = sizeof(DotKNdArray) + dataBytes;

    DotKNdArray *arr = (DotKNdArray *)malloc(totalBytes);
    if (arr == NULL)
    {
        g_api->raiseError("Failed to allocate ndarray memory");
        *hasError = true;
        return NULL;
    }

    arr->magic = NDARRAY_MAGIC;
    arr->dtype = dtype;
    arr->ndim = ndim;
    arr->size = size;

    for (int i = 0; i < NDARRAY_MAX_DIMS; i++)
    {
        arr->shape[i] = 0;
        arr->strides[i] = 0;
    }

    for (int i = 0; i < ndim; i++)
        arr->shape[i] = shape[i];

    arr->strides[ndim - 1] = 1;
    for (int i = ndim - 2; i >= 0; i--)
        arr->strides[i] = arr->strides[i + 1] * arr->shape[i + 1];

    memset(arr->data, 0, dataBytes);
    return arr;
}

static bool try_get_ndarray(Value v, DotKNdArray **out, const char *name, int index, bool *hasError)
{
    if (!IS_FOREIGN_TYPE(v, TYPE_NDARRAY))
    {
        g_api->raiseError("%s expects argument %d to be ndarray", name, index);
        *hasError = true;
        return false;
    }

    ObjForeign *f = AS_FOREIGN(v);
    if (f->ptr == NULL)
    {
        g_api->raiseError("%s received an invalid ndarray pointer", name);
        *hasError = true;
        return false;
    }

    DotKNdArray *arr = (DotKNdArray *)f->ptr;
    if (arr->magic != NDARRAY_MAGIC)
    {
        g_api->raiseError("%s received a foreign value that is not an ndarray", name);
        *hasError = true;
        return false;
    }

    *out = arr;
    return true;
}

static double ndarray_read_as_double(const DotKNdArray *arr, int64_t idx)
{
    if (arr->dtype == ND_DTYPE_INT64)
        return (double)((int64_t *)arr->data)[idx];
    return ((double *)arr->data)[idx];
}

static void ndarray_write_from_double(DotKNdArray *arr, int64_t idx, double v)
{
    if (arr->dtype == ND_DTYPE_INT64)
    {
        ((int64_t *)arr->data)[idx] = (int64_t)v;
        return;
    }
    ((double *)arr->data)[idx] = v;
}

static Value make_ndarray_value(DotKNdArray *arr)
{
    return g_api->makeForeign(TYPE_NDARRAY, arr, true);
}

static Value nd_zeros_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (argc != 1 && argc != 2)
    {
        g_api->raiseError("nd_zeros(shape[, dtype]) expects 1 or 2 arguments but got %d", argc);
        *hasError = true;
        return NIL_VAL;
    }

    int dtype = ND_DTYPE_FLOAT64;
    if (argc == 2 && !parse_dtype_from_optional(argv[1], &dtype, "nd_zeros(shape[, dtype])", 2, hasError))
        return NIL_VAL;

    int ndim = 0;
    int64_t shape[NDARRAY_MAX_DIMS];
    int64_t size = 0;
    if (!parse_shape_list(argv[0], &ndim, shape, &size, "nd_zeros(shape[, dtype])", hasError))
        return NIL_VAL;

    DotKNdArray *arr = ndarray_alloc(dtype, ndim, shape, size, hasError);
    if (arr == NULL)
        return NIL_VAL;

    return make_ndarray_value(arr);
}

static Value nd_ones_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (argc != 1 && argc != 2)
    {
        g_api->raiseError("nd_ones(shape[, dtype]) expects 1 or 2 arguments but got %d", argc);
        *hasError = true;
        return NIL_VAL;
    }

    int dtype = ND_DTYPE_FLOAT64;
    if (argc == 2 && !parse_dtype_from_optional(argv[1], &dtype, "nd_ones(shape[, dtype])", 2, hasError))
        return NIL_VAL;

    int ndim = 0;
    int64_t shape[NDARRAY_MAX_DIMS];
    int64_t size = 0;
    if (!parse_shape_list(argv[0], &ndim, shape, &size, "nd_ones(shape[, dtype])", hasError))
        return NIL_VAL;

    DotKNdArray *arr = ndarray_alloc(dtype, ndim, shape, size, hasError);
    if (arr == NULL)
        return NIL_VAL;

    for (int64_t i = 0; i < size; i++)
        ndarray_write_from_double(arr, i, 1.0);

    return make_ndarray_value(arr);
}

static Value nd_arange_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (argc != 3 && argc != 4)
    {
        g_api->raiseError("nd_arange(start, stop, step[, dtype]) expects 3 or 4 arguments but got %d", argc);
        *hasError = true;
        return NIL_VAL;
    }

    if (!expect_number(argv[0], "nd_arange(start, stop, step[, dtype])", 1, hasError) ||
        !expect_number(argv[1], "nd_arange(start, stop, step[, dtype])", 2, hasError) ||
        !expect_number(argv[2], "nd_arange(start, stop, step[, dtype])", 3, hasError))
        return NIL_VAL;

    double start = AS_NUM(argv[0]);
    double stop = AS_NUM(argv[1]);
    double step = AS_NUM(argv[2]);
    if (step == 0.0)
    {
        g_api->raiseError("nd_arange step cannot be zero");
        *hasError = true;
        return NIL_VAL;
    }

    int dtype = ND_DTYPE_FLOAT64;
    if (argc == 4 && !parse_dtype_from_optional(argv[3], &dtype, "nd_arange(start, stop, step[, dtype])", 4, hasError))
        return NIL_VAL;

    int64_t count = 0;
    if (step > 0)
    {
        for (double x = start; x < stop; x += step)
            count++;
    }
    else
    {
        for (double x = start; x > stop; x += step)
            count++;
    }

    int64_t shape[1] = {count};
    DotKNdArray *arr = ndarray_alloc(dtype, 1, shape, count, hasError);
    if (arr == NULL)
        return NIL_VAL;

    double x = start;
    for (int64_t i = 0; i < count; i++)
    {
        ndarray_write_from_double(arr, i, x);
        x += step;
    }

    return make_ndarray_value(arr);
}

static Value nd_shape_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_shape(arr)", argc, 1, hasError))
        return NIL_VAL;

    DotKNdArray *arr = NULL;
    if (!try_get_ndarray(argv[0], &arr, "nd_shape(arr)", 1, hasError))
        return NIL_VAL;

    Value out = g_api->makeList();
    g_api->pushValue(out);
    for (int i = 0; i < arr->ndim; i++)
        g_api->listAppend(out, NUM_VAL((double)arr->shape[i]));
    g_api->popValue();
    return out;
}

static Value nd_dtype_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_dtype(arr)", argc, 1, hasError))
        return NIL_VAL;

    DotKNdArray *arr = NULL;
    if (!try_get_ndarray(argv[0], &arr, "nd_dtype(arr)", 1, hasError))
        return NIL_VAL;

    if (arr->dtype == ND_DTYPE_INT64)
        return g_api->makeString("int64", 5, false);
    return g_api->makeString("float64", 7, false);
}

typedef struct
{
    int ndim;
    int leafDepth;
    int64_t shape[NDARRAY_MAX_DIMS];
} NdInferCtx;

static bool infer_nested_shape(Value node, int depth, NdInferCtx *ctx, const char *name, bool *hasError)
{
    if (IS_LIST(node))
    {
        if (depth >= NDARRAY_MAX_DIMS)
        {
            g_api->raiseError("%s rank exceeds max supported dims (%d)", name, NDARRAY_MAX_DIMS);
            *hasError = true;
            return false;
        }

        ObjList *list = AS_LIST(node);
        if (list->count <= 0)
        {
            g_api->raiseError("%s does not accept empty nested lists", name);
            *hasError = true;
            return false;
        }

        if (ctx->leafDepth != -1 && depth >= ctx->leafDepth)
        {
            g_api->raiseError("%s has inconsistent nesting depth", name);
            *hasError = true;
            return false;
        }

        if (ctx->shape[depth] == 0)
            ctx->shape[depth] = list->count;
        else if (ctx->shape[depth] != list->count)
        {
            g_api->raiseError("%s nested lists must have rectangular shape", name);
            *hasError = true;
            return false;
        }

        if (depth + 1 > ctx->ndim)
            ctx->ndim = depth + 1;

        for (int i = 0; i < list->count; i++)
        {
            if (!infer_nested_shape(list->items[i], depth + 1, ctx, name, hasError))
                return false;
        }
        return true;
    }

    if (!IS_NUM(node))
    {
        g_api->raiseError("%s expects nested lists of numbers", name);
        *hasError = true;
        return false;
    }

    if (ctx->leafDepth == -1)
        ctx->leafDepth = depth;
    else if (ctx->leafDepth != depth)
    {
        g_api->raiseError("%s has inconsistent scalar nesting depth", name);
        *hasError = true;
        return false;
    }

    return true;
}

static bool infer_array_shape_and_rank(Value data, int *outNdim, int64_t outShape[NDARRAY_MAX_DIMS], const char *name, bool *hasError)
{
    if (!IS_LIST(data))
    {
        g_api->raiseError("%s expects a List or nested List", name);
        *hasError = true;
        return false;
    }

    NdInferCtx ctx;
    ctx.ndim = 0;
    ctx.leafDepth = -1;
    for (int i = 0; i < NDARRAY_MAX_DIMS; i++)
        ctx.shape[i] = 0;

    if (!infer_nested_shape(data, 0, &ctx, name, hasError))
        return false;

    if (ctx.leafDepth <= 0)
    {
        g_api->raiseError("%s must contain at least one dimension", name);
        *hasError = true;
        return false;
    }

    *outNdim = ctx.leafDepth;
    for (int i = 0; i < *outNdim; i++)
        outShape[i] = ctx.shape[i];
    return true;
}

static bool fill_array_from_nested(Value node, DotKNdArray *arr, int depth, int64_t *cursor, const char *name, bool *hasError)
{
    if (depth == arr->ndim)
    {
        if (!IS_NUM(node))
        {
            g_api->raiseError("%s expects scalar leaves to be numbers", name);
            *hasError = true;
            return false;
        }
        ndarray_write_from_double(arr, *cursor, AS_NUM(node));
        (*cursor)++;
        return true;
    }

    if (!IS_LIST(node))
    {
        g_api->raiseError("%s has inconsistent nesting while filling array", name);
        *hasError = true;
        return false;
    }

    ObjList *list = AS_LIST(node);
    if (list->count != arr->shape[depth])
    {
        g_api->raiseError("%s encountered ragged nested list while filling array", name);
        *hasError = true;
        return false;
    }

    for (int i = 0; i < list->count; i++)
    {
        if (!fill_array_from_nested(list->items[i], arr, depth + 1, cursor, name, hasError))
            return false;
    }
    return true;
}

static Value nd_array_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (argc != 1 && argc != 2)
    {
        g_api->raiseError("nd_array(data[, dtype]) expects 1 or 2 arguments but got %d", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!expect_list(argv[0], "nd_array(data[, dtype])", 1, hasError))
        return NIL_VAL;

    int dtype = ND_DTYPE_FLOAT64;
    if (argc == 2 && !parse_dtype_from_optional(argv[1], &dtype, "nd_array(data[, dtype])", 2, hasError))
        return NIL_VAL;

    int ndim = 0;
    int64_t shape[NDARRAY_MAX_DIMS] = {0};
    if (!infer_array_shape_and_rank(argv[0], &ndim, shape, "nd_array(data[, dtype])", hasError))
        return NIL_VAL;

    int64_t size = 1;
    for (int i = 0; i < ndim; i++)
        size *= shape[i];

    DotKNdArray *arr = ndarray_alloc(dtype, ndim, shape, size, hasError);
    if (arr == NULL)
        return NIL_VAL;

    int64_t idx = 0;
    if (!fill_array_from_nested(argv[0], arr, 0, &idx, "nd_array(data[, dtype])", hasError))
    {
        free(arr);
        return NIL_VAL;
    }

    return make_ndarray_value(arr);
}

static Value nd_reshape_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_reshape(arr, shape)", argc, 2, hasError))
        return NIL_VAL;

    DotKNdArray *src = NULL;
    if (!try_get_ndarray(argv[0], &src, "nd_reshape(arr, shape)", 1, hasError))
        return NIL_VAL;

    int ndim = 0;
    int64_t shape[NDARRAY_MAX_DIMS];
    int64_t size = 0;
    if (!parse_shape_list(argv[1], &ndim, shape, &size, "nd_reshape(arr, shape)", hasError))
        return NIL_VAL;

    if (size != src->size)
    {
        g_api->raiseError("nd_reshape(arr, shape) cannot reshape size %lld into %lld", (long long)src->size, (long long)size);
        *hasError = true;
        return NIL_VAL;
    }

    DotKNdArray *dst = ndarray_alloc(src->dtype, ndim, shape, size, hasError);
    if (dst == NULL)
        return NIL_VAL;

    memcpy(dst->data, src->data, (size_t)size * dtype_elem_size(src->dtype));
    return make_ndarray_value(dst);
}

static bool same_shape(const DotKNdArray *a, const DotKNdArray *b)
{
    if (a->ndim != b->ndim)
        return false;
    for (int i = 0; i < a->ndim; i++)
    {
        if (a->shape[i] != b->shape[i])
            return false;
    }
    return true;
}

static bool is_integral_number(Value v)
{
    if (!IS_NUM(v))
        return false;
    double d = AS_NUM(v);
    int64_t i = (int64_t)d;
    return d == (double)i;
}

static bool calc_broadcast_shape(const DotKNdArray *a, const DotKNdArray *b,
                                 int *outNdim, int64_t outShape[NDARRAY_MAX_DIMS],
                                 const char *name, bool *hasError)
{
    int ndim = (a->ndim > b->ndim) ? a->ndim : b->ndim;
    int aShift = ndim - a->ndim;
    int bShift = ndim - b->ndim;

    for (int i = 0; i < ndim; i++)
    {
        int64_t aDim = (i - aShift >= 0) ? a->shape[i - aShift] : 1;
        int64_t bDim = (i - bShift >= 0) ? b->shape[i - bShift] : 1;
        if (aDim != bDim && aDim != 1 && bDim != 1)
        {
            g_api->raiseError("%s cannot broadcast shapes", name);
            *hasError = true;
            return false;
        }
        outShape[i] = (aDim > bDim) ? aDim : bDim;
    }

    *outNdim = ndim;
    return true;
}

static int64_t shape_size_or_error(const int64_t *shape, int ndim, const char *name, bool *hasError)
{
    int64_t size = 1;
    for (int i = 0; i < ndim; i++)
    {
        if (shape[i] <= 0)
        {
            g_api->raiseError("%s produced invalid shape", name);
            *hasError = true;
            return 0;
        }
        if (size > INT64_MAX / shape[i])
        {
            g_api->raiseError("%s shape is too large", name);
            *hasError = true;
            return 0;
        }
        size *= shape[i];
    }
    return size;
}

static int64_t offset_for_broadcast_operand(const DotKNdArray *arr,
                                            const int64_t *outShape,
                                            int outNdim,
                                            int64_t linear)
{
    int64_t offset = 0;
    int shift = outNdim - arr->ndim;

    for (int dim = outNdim - 1; dim >= 0; dim--)
    {
        int64_t coord = linear % outShape[dim];
        linear /= outShape[dim];

        int arrDim = dim - shift;
        if (arrDim >= 0)
        {
            int64_t useCoord = (arr->shape[arrDim] == 1) ? 0 : coord;
            offset += useCoord * arr->strides[arrDim];
        }
    }

    return offset;
}

static Value nd_add_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_add(a, b)", argc, 2, hasError))
        return NIL_VAL;

    DotKNdArray *a = NULL;
    DotKNdArray *b = NULL;
    bool aIsArr = IS_FOREIGN_TYPE(argv[0], TYPE_NDARRAY);
    bool bIsArr = IS_FOREIGN_TYPE(argv[1], TYPE_NDARRAY);

    if (!aIsArr && !bIsArr)
    {
        g_api->raiseError("nd_add(a, b) expects at least one ndarray operand");
        *hasError = true;
        return NIL_VAL;
    }

    if (aIsArr && bIsArr)
    {
        if (!try_get_ndarray(argv[0], &a, "nd_add(a, b)", 1, hasError) ||
            !try_get_ndarray(argv[1], &b, "nd_add(a, b)", 2, hasError))
            return NIL_VAL;

        int outNdim = 0;
        int64_t outShape[NDARRAY_MAX_DIMS];
        if (!calc_broadcast_shape(a, b, &outNdim, outShape, "nd_add(a, b)", hasError))
            return NIL_VAL;

        int64_t outSize = shape_size_or_error(outShape, outNdim, "nd_add(a, b)", hasError);
        if (*hasError)
            return NIL_VAL;

        int outDtype = (a->dtype == ND_DTYPE_INT64 && b->dtype == ND_DTYPE_INT64) ? ND_DTYPE_INT64 : ND_DTYPE_FLOAT64;
        DotKNdArray *out = ndarray_alloc(outDtype, outNdim, outShape, outSize, hasError);
        if (out == NULL)
            return NIL_VAL;

        for (int64_t i = 0; i < outSize; i++)
        {
            int64_t aOff = offset_for_broadcast_operand(a, outShape, outNdim, i);
            int64_t bOff = offset_for_broadcast_operand(b, outShape, outNdim, i);
            ndarray_write_from_double(out, i, ndarray_read_as_double(a, aOff) + ndarray_read_as_double(b, bOff));
        }

        return make_ndarray_value(out);
    }

    DotKNdArray *arr = NULL;
    Value scalarV;
    const char *argName;
    if (aIsArr)
    {
        if (!try_get_ndarray(argv[0], &arr, "nd_add(a, b)", 1, hasError))
            return NIL_VAL;
        scalarV = argv[1];
        argName = "nd_add(a, b)";
    }
    else
    {
        if (!try_get_ndarray(argv[1], &arr, "nd_add(a, b)", 2, hasError))
            return NIL_VAL;
        scalarV = argv[0];
        argName = "nd_add(a, b)";
    }

    if (!IS_NUM(scalarV))
    {
        g_api->raiseError("%s scalar operand must be Number", argName);
        *hasError = true;
        return NIL_VAL;
    }

    double scalar = AS_NUM(scalarV);
    int outDtype = (arr->dtype == ND_DTYPE_INT64 && is_integral_number(scalarV)) ? ND_DTYPE_INT64 : ND_DTYPE_FLOAT64;
    DotKNdArray *out = ndarray_alloc(outDtype, arr->ndim, arr->shape, arr->size, hasError);
    if (out == NULL)
        return NIL_VAL;

    for (int64_t i = 0; i < arr->size; i++)
        ndarray_write_from_double(out, i, ndarray_read_as_double(arr, i) + scalar);

    return make_ndarray_value(out);
}

static Value nd_matmul_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_matmul(a, b)", argc, 2, hasError))
        return NIL_VAL;

    DotKNdArray *a = NULL;
    DotKNdArray *b = NULL;
    if (!try_get_ndarray(argv[0], &a, "nd_matmul(a, b)", 1, hasError) ||
        !try_get_ndarray(argv[1], &b, "nd_matmul(a, b)", 2, hasError))
        return NIL_VAL;

    if (a->ndim != 2 || b->ndim != 2)
    {
        g_api->raiseError("nd_matmul(a, b) currently supports 2D arrays only");
        *hasError = true;
        return NIL_VAL;
    }

    int64_t m = a->shape[0];
    int64_t kA = a->shape[1];
    int64_t kB = b->shape[0];
    int64_t n = b->shape[1];
    if (kA != kB)
    {
        g_api->raiseError("nd_matmul(a, b) dimension mismatch: %lld != %lld", (long long)kA, (long long)kB);
        *hasError = true;
        return NIL_VAL;
    }

    int64_t outShape[2] = {m, n};
    int outDtype = (a->dtype == ND_DTYPE_INT64 && b->dtype == ND_DTYPE_INT64) ? ND_DTYPE_INT64 : ND_DTYPE_FLOAT64;
    DotKNdArray *out = ndarray_alloc(outDtype, 2, outShape, m * n, hasError);
    if (out == NULL)
        return NIL_VAL;

    for (int64_t i = 0; i < m; i++)
    {
        for (int64_t j = 0; j < n; j++)
        {
            double sum = 0.0;
            for (int64_t k = 0; k < kA; k++)
            {
                int64_t aIdx = i * a->strides[0] + k * a->strides[1];
                int64_t bIdx = k * b->strides[0] + j * b->strides[1];
                sum += ndarray_read_as_double(a, aIdx) * ndarray_read_as_double(b, bIdx);
            }
            ndarray_write_from_double(out, i * out->strides[0] + j * out->strides[1], sum);
        }
    }

    return make_ndarray_value(out);
}

static Value nd_transpose_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_transpose(arr)", argc, 1, hasError))
        return NIL_VAL;

    DotKNdArray *src = NULL;
    if (!try_get_ndarray(argv[0], &src, "nd_transpose(arr)", 1, hasError))
        return NIL_VAL;

    if (src->ndim != 2)
    {
        g_api->raiseError("nd_transpose(arr) currently supports 2D arrays only");
        *hasError = true;
        return NIL_VAL;
    }

    int64_t outShape[2] = {src->shape[1], src->shape[0]};
    DotKNdArray *out = ndarray_alloc(src->dtype, 2, outShape, src->size, hasError);
    if (out == NULL)
        return NIL_VAL;

    for (int64_t r = 0; r < src->shape[0]; r++)
    {
        for (int64_t c = 0; c < src->shape[1]; c++)
        {
            int64_t srcIdx = r * src->strides[0] + c * src->strides[1];
            int64_t dstIdx = c * out->strides[0] + r * out->strides[1];
            ndarray_write_from_double(out, dstIdx, ndarray_read_as_double(src, srcIdx));
        }
    }

    return make_ndarray_value(out);
}

static Value nd_sum_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (argc != 1 && argc != 2)
    {
        g_api->raiseError("nd_sum(arr[, axis]) expects 1 or 2 arguments but got %d", argc);
        *hasError = true;
        return NIL_VAL;
    }

    DotKNdArray *src = NULL;
    if (!try_get_ndarray(argv[0], &src, "nd_sum(arr[, axis])", 1, hasError))
        return NIL_VAL;

    if (argc == 1)
    {
        double sum = 0.0;
        for (int64_t i = 0; i < src->size; i++)
            sum += ndarray_read_as_double(src, i);
        return NUM_VAL(sum);
    }

    if (!IS_NUM(argv[1]))
    {
        g_api->raiseError("nd_sum(arr[, axis]) axis must be Number");
        *hasError = true;
        return NIL_VAL;
    }

    int64_t axis = (int64_t)AS_NUM(argv[1]);
    if ((double)axis != AS_NUM(argv[1]))
    {
        g_api->raiseError("nd_sum(arr[, axis]) axis must be an integer");
        *hasError = true;
        return NIL_VAL;
    }

    if (axis < 0)
        axis += src->ndim;
    if (axis < 0 || axis >= src->ndim)
    {
        g_api->raiseError("nd_sum(arr[, axis]) axis out of bounds");
        *hasError = true;
        return NIL_VAL;
    }

    if (src->ndim == 1)
    {
        double sum = 0.0;
        for (int64_t i = 0; i < src->size; i++)
            sum += ndarray_read_as_double(src, i);
        return NUM_VAL(sum);
    }

    int outNdim = src->ndim - 1;
    int64_t outShape[NDARRAY_MAX_DIMS];
    for (int i = 0, j = 0; i < src->ndim; i++)
    {
        if (i == axis)
            continue;
        outShape[j++] = src->shape[i];
    }

    int64_t outSize = shape_size_or_error(outShape, outNdim, "nd_sum(arr[, axis])", hasError);
    if (*hasError)
        return NIL_VAL;

    DotKNdArray *out = ndarray_alloc(ND_DTYPE_FLOAT64, outNdim, outShape, outSize, hasError);
    if (out == NULL)
        return NIL_VAL;

    for (int64_t linear = 0; linear < outSize; linear++)
    {
        int64_t tmp = linear;
        int64_t srcBase = 0;
        for (int d = outNdim - 1; d >= 0; d--)
        {
            int64_t coord = tmp % outShape[d];
            tmp /= outShape[d];
            int srcDim = (d < axis) ? d : d + 1;
            srcBase += coord * src->strides[srcDim];
        }

        double sum = 0.0;
        for (int64_t k = 0; k < src->shape[axis]; k++)
            sum += ndarray_read_as_double(src, srcBase + k * src->strides[axis]);
        ndarray_write_from_double(out, linear, sum);
    }

    return make_ndarray_value(out);
}

static bool calc_flat_index(const DotKNdArray *arr, Value indicesVal, int64_t *flatOut, const char *name, bool *hasError)
{
    if (!IS_LIST(indicesVal))
    {
        g_api->raiseError("%s expects indices to be a List", name);
        *hasError = true;
        return false;
    }

    ObjList *indices = AS_LIST(indicesVal);
    if (indices->count != arr->ndim)
    {
        g_api->raiseError("%s expects %d index values but got %d", name, arr->ndim, indices->count);
        *hasError = true;
        return false;
    }

    int64_t flat = 0;
    for (int i = 0; i < arr->ndim; i++)
    {
        Value idxV = indices->items[i];
        if (!IS_NUM(idxV))
        {
            g_api->raiseError("%s index entries must be numbers", name);
            *hasError = true;
            return false;
        }

        int64_t idx = (int64_t)AS_NUM(idxV);
        if ((double)idx != AS_NUM(idxV))
        {
            g_api->raiseError("%s index entries must be integers", name);
            *hasError = true;
            return false;
        }

        if (idx < 0)
            idx += arr->shape[i];
        if (idx < 0 || idx >= arr->shape[i])
        {
            g_api->raiseError("%s index out of bounds at dim %d", name, i);
            *hasError = true;
            return false;
        }

        flat += idx * arr->strides[i];
    }

    *flatOut = flat;
    return true;
}

static Value nd_get_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_get(arr, indices)", argc, 2, hasError))
        return NIL_VAL;

    DotKNdArray *arr = NULL;
    if (!try_get_ndarray(argv[0], &arr, "nd_get(arr, indices)", 1, hasError))
        return NIL_VAL;

    int64_t flat = 0;
    if (!calc_flat_index(arr, argv[1], &flat, "nd_get(arr, indices)", hasError))
        return NIL_VAL;

    return NUM_VAL(ndarray_read_as_double(arr, flat));
}

static Value nd_set_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_set(arr, indices, value)", argc, 3, hasError))
        return NIL_VAL;

    DotKNdArray *arr = NULL;
    if (!try_get_ndarray(argv[0], &arr, "nd_set(arr, indices, value)", 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[2], "nd_set(arr, indices, value)", 3, hasError))
        return NIL_VAL;

    int64_t flat = 0;
    if (!calc_flat_index(arr, argv[1], &flat, "nd_set(arr, indices, value)", hasError))
        return NIL_VAL;

    ndarray_write_from_double(arr, flat, AS_NUM(argv[2]));
    return NIL_VAL;
}

static Value nd_to_list_recursive(const DotKNdArray *src, int depth, int64_t baseOffset)
{
    Value out = g_api->makeList();
    g_api->pushValue(out);

    if (depth == src->ndim - 1)
    {
        for (int64_t i = 0; i < src->shape[depth]; i++)
        {
            int64_t idx = baseOffset + i * src->strides[depth];
            g_api->listAppend(out, NUM_VAL(ndarray_read_as_double(src, idx)));
        }
    }
    else
    {
        for (int64_t i = 0; i < src->shape[depth]; i++)
        {
            Value child = nd_to_list_recursive(src, depth + 1, baseOffset + i * src->strides[depth]);
            g_api->pushValue(child);
            g_api->listAppend(out, child);
            g_api->popValue();
        }
    }

    g_api->popValue();
    return out;
}

static Value nd_to_list_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("nd_to_list(arr)", argc, 1, hasError))
        return NIL_VAL;

    DotKNdArray *arr = NULL;
    if (!try_get_ndarray(argv[0], &arr, "nd_to_list(arr)", 1, hasError))
        return NIL_VAL;

    return nd_to_list_recursive(arr, 0, 0);
}

bool dotk_init_module(const DotKNativeApi *api)
{
    if (api == NULL)
        return false;
    if (api->version != DOTK_NATIVE_API_VERSION)
        return false;
    if (api->makeForeign == NULL)
        return false;

    g_api = api;

    g_api->defineNative("nd_array", nd_array_native);
    g_api->defineNative("nd_zeros", nd_zeros_native);
    g_api->defineNative("nd_ones", nd_ones_native);
    g_api->defineNative("nd_arange", nd_arange_native);
    g_api->defineNative("nd_shape", nd_shape_native);
    g_api->defineNative("nd_dtype", nd_dtype_native);
    g_api->defineNative("nd_reshape", nd_reshape_native);
    g_api->defineNative("nd_add", nd_add_native);
    g_api->defineNative("nd_matmul", nd_matmul_native);
    g_api->defineNative("nd_transpose", nd_transpose_native);
    g_api->defineNative("nd_sum", nd_sum_native);
    g_api->defineNative("nd_get", nd_get_native);
    g_api->defineNative("nd_set", nd_set_native);
    g_api->defineNative("nd_to_list", nd_to_list_native);

    return true;
}
