#define _POSIX_C_SOURCE 200809L
#include "dotk.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

static double seconds(void)
{
    struct timespec instant;
    clock_gettime(CLOCK_MONOTONIC, &instant);
    return (double)instant.tv_sec + (double)instant.tv_nsec / 1000000000.0;
}

static int check(DotKVM *context, DotKStatus status)
{
    if (status == DOTK_OK)
        return 0;
    const DotKDiagnostic *diagnostic = dotkGetDiagnostic(context);
    fprintf(stderr, "Embedding benchmark failed: %s\n", diagnostic->message);
    return 1;
}

int main(void)
{
    const int iterations = 5000;
    const char *source = "var benchmarkResult = replace('ABC', 'A', 'Z')\n";
    DotKVM *context = dotkCreate(NULL);
    if (!context)
        return 1;
    DotKHandle *program = NULL;
    DotKHandle *result = NULL;
    DotKHandle *retained[2000] = {0};
    for (int index = 0; index < 2000; index++)
    {
        char text[64];
        int length = snprintf(text, sizeof(text), "retained-state-%d", index);
        if (check(context, dotkMakeString(context, text, (size_t)length, &retained[index])))
            goto failure;
    }
    if (check(context, dotkCompile(context, source, "benchmark.k", &program)))
        goto failure;
    for (int warmup = 0; warmup < 10; warmup++)
        if (check(context, dotkEval(context, source, "benchmark.k")))
            goto failure;
    double start = seconds();
    for (int iteration = 0; iteration < iterations; iteration++)
        if (check(context, dotkEval(context, source, "benchmark.k")))
            goto failure;
    double evaluation = seconds() - start;
    DotKProfileStats evaluationStats = {0};
    evaluationStats.size = sizeof(evaluationStats);
    if (check(context, dotkGetProfileStats(context, &evaluationStats)))
        goto failure;
    if (check(context, dotkCollect(context)))
        goto failure;
    start = seconds();
    for (int iteration = 0; iteration < iterations; iteration++)
    {
        if (check(context, dotkCall(context, program, 0, NULL, &result)))
            goto failure;
        if (check(context, dotkRelease(result)))
            goto failure;
        result = NULL;
    }
    double execution = seconds() - start;
    printf("iterations,eval_seconds,compiled_seconds,eval_over_compiled\n");
    printf("%d,%.6f,%.6f,%.2f\n", iterations, evaluation, execution, evaluation / execution);
        printf("eval_allocation_calls,eval_allocation_probes,eval_collections\n");
        printf("%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", evaluationStats.allocation_calls,
            evaluationStats.allocation_probes, evaluationStats.collections);
    dotkRelease(program);
    for (int index = 0; index < 2000; index++)
        dotkRelease(retained[index]);
    return dotkDestroy(context) == DOTK_OK ? 0 : 1;

failure:
    dotkDestroy(context);
    return 1;
}