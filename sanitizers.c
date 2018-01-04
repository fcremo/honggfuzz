#include "sanitizers.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libcommon/common.h"
#include "libcommon/files.h"
#include "libcommon/log.h"
#include "libcommon/util.h"

/* Stringify */
#define XSTR(x) #x
#define STR(x) XSTR(x)

/*
 * All clang sanitizers, except ASan, can be activated for target binaries
 * with or without the matching runtime library (libcompiler_rt). If runtime
 * libraries are included in target fuzzing environment, we can benefit from the
 * various Die() callbacks and abort/exit logic manipulation. However, some
 * setups (e.g. Android production ARM/ARM64 devices) enable sanitizers, such as
 * UBSan, without the runtime libraries. As such, their default ftrap is activated
 * which is for most cases a SIGABRT. For these cases end-user needs to enable
 * SIGABRT monitoring flag, otherwise these crashes will be missed.
 *
 * Normally SIGABRT is not a wanted signal to monitor for Android, since it produces
 * lots of useless crashes due to way Android process termination hacks work. As
 * a result the sanitizer's 'abort_on_error' flag cannot be utilized since it
 * invokes abort() internally. In order to not lose crashes a custom exitcode can
 * be registered and monitored. Since exitcode is a global flag, it's assumed
 * that target is compiled with only one sanitizer type enabled at a time.
 *
 * For cases where clang runtime library linking is not an option, SIGABRT should
 * be monitored even for noisy targets, such as the Android OS, since no viable
 * alternative exists.
 *
 * There might be cases where ASan instrumented targets crash while generating
 * reports for detected errors (inside __asan_report_error() proc). Under such
 * scenarios target fails to exit or SIGABRT (AsanDie() proc) as defined in
 * ASAN_OPTIONS flags, leaving garbage logs. An attempt is made to parse such
 * logs for cases where enough data are written to identify potentially missed
 * crashes. If ASan internal error results into a SIGSEGV being raised, it
 * will get caught from ptrace API, handling the discovered ASan internal crash.
 */

/* 'log_path' output directory for sanitizer reports */
#define kSANLOGDIR "log_path="

/* 'coverage_dir' output directory for coverage data files is set dynamically */
#define kSANCOVDIR "coverage_dir="

/* Raise SIGABRT on error or continue with exitcode logic */
#define kABORT_ENABLED "abort_on_error=1"
#define kABORT_DISABLED "abort_on_error=0"

/*
 * Common sanitizer flags
 *
 * symbolize: Disable symbolication since it changes logs (which are parsed) format
 */
#define kSAN_COMMON "symbolize=0"

/* --{ ASan }-- */
/*
 *Sanitizer specific flags (notice that if enabled 'abort_on_error' has priority
 * over exitcode')
 */
#define kASAN_COMMON_OPTS        \
    "allow_user_segv_handler=1:" \
    "handle_segv=0:"             \
    "allocator_may_return_null=1:" kSAN_COMMON ":exitcode=" STR(HF_SAN_EXIT_CODE)
/* Platform specific flags */
#if defined(__ANDROID__)
/*
 * start_deactivated: Enable on Android to reduce memory usage (useful when not all
 *                    target's DSOs are compiled with sanitizer enabled
 */
#define kASAN_OPTS kASAN_COMMON_OPTS ":start_deactivated=1"
#else
#define kASAN_OPTS kASAN_COMMON_OPTS
#endif

/* --{ UBSan }-- */
#define kUBSAN_OPTS kSAN_COMMON ":exitcode=" STR(HF_SAN_EXIT_CODE)

/* --{ MSan }-- */
#define kMSAN_OPTS \
    kSAN_COMMON ":exit_code=" STR(HF_SAN_EXIT_CODE) ":"                                            \
                                                    "wrap_signals=0:print_stats=1"

/* If no sanitzer support was requested, simply make it use abort() on errors */
#define kSAN_REGULAR                                                        \
    "abort_on_error=1:handle_segv=0:handle_sigbus=0:handle_abort=0:"        \
    "handle_sigill=0:handle_sigfpe=0:allocator_may_return_null=1:"          \
    "symbolize=1:detect_leaks=0:disable_coredump=0:soft_rss_limit_mb=8192"

/*
 * If the program ends with a signal that ASan does not handle (or can not
 * handle at all, like SIGKILL), coverage data will be lost. This is a big
 * problem on Android, where SIGKILL is a normal way of evicting applications
 * from memory. With 'coverage_direct=1' coverage data is written to a
 * memory-mapped file as soon as it collected. Non-Android targets can disable
 * coverage direct when more coverage data collection methods are implemented.
 */
#define kSAN_COV_OPTS "coverage=1:coverage_direct=1"

static void sanitizers_AddToEnv(honggfuzz_t* hfuzz, const char* prefix, char* env) {
    for (size_t i = 0; i < ARRAYSIZE(hfuzz->exe.envs); i++) {
        if (hfuzz->exe.envs[i] == NULL) {
            hfuzz->exe.envs[i] = env;
            break;
        }
        /* If the env already exist, skip overriding it */
        if (strncmp(hfuzz->exe.envs[i], prefix, strlen(prefix)) == 0) {
            break;
        }
    }
    LOG_D("%s", env);
}

bool sanitizers_Init(honggfuzz_t* hfuzz) {
    if (hfuzz->linux.pid > 0) {
        return true;
    }

    char* abortFlag;
    if (hfuzz->monitorSIGABRT) {
        abortFlag = kABORT_ENABLED;
    } else {
        abortFlag = kABORT_DISABLED;
    }

    /* Address Sanitizer (ASan) */
    if (!hfuzz->enableSanitizers) {
        snprintf(
            hfuzz->sanOpts.asanOpts, sizeof(hfuzz->sanOpts.asanOpts), "ASAN_OPTIONS=" kSAN_REGULAR);
    } else if (hfuzz->useSanCov) {
        snprintf(hfuzz->sanOpts.asanOpts, sizeof(hfuzz->sanOpts.asanOpts),
            "ASAN_OPTIONS=%s:%s:%s:%s%s/%s:%s%s/%s", kASAN_OPTS, abortFlag, kSAN_COV_OPTS,
            kSANCOVDIR, hfuzz->io.workDir, _HF_SANCOV_DIR, kSANLOGDIR, hfuzz->io.workDir,
            kLOGPREFIX);
    } else {
        snprintf(hfuzz->sanOpts.asanOpts, sizeof(hfuzz->sanOpts.asanOpts),
            "ASAN_OPTIONS=%s:%s:%s%s/%s", kASAN_OPTS, abortFlag, kSANLOGDIR, hfuzz->io.workDir,
            kLOGPREFIX);
    }
    sanitizers_AddToEnv(hfuzz, "ASAN_OPTIONS=", hfuzz->sanOpts.asanOpts);

    /* Undefined Behavior Sanitizer (UBSan) */
    if (!hfuzz->enableSanitizers) {
        snprintf(hfuzz->sanOpts.ubsanOpts, sizeof(hfuzz->sanOpts.ubsanOpts),
            "UBSAN_OPTIONS=" kSAN_REGULAR);
    } else if (hfuzz->useSanCov) {
        snprintf(hfuzz->sanOpts.ubsanOpts, sizeof(hfuzz->sanOpts.ubsanOpts),
            "UBSAN_OPTIONS=%s:%s:%s:%s%s/%s:%s%s/%s", kUBSAN_OPTS, abortFlag, kSAN_COV_OPTS,
            kSANCOVDIR, hfuzz->io.workDir, _HF_SANCOV_DIR, kSANLOGDIR, hfuzz->io.workDir,
            kLOGPREFIX);
    } else {
        snprintf(hfuzz->sanOpts.ubsanOpts, sizeof(hfuzz->sanOpts.ubsanOpts),
            "UBSAN_OPTIONS=%s:%s:%s%s/%s", kUBSAN_OPTS, abortFlag, kSANLOGDIR, hfuzz->io.workDir,
            kLOGPREFIX);
    }
    sanitizers_AddToEnv(hfuzz, "UBSAN_OPTIONS=", hfuzz->sanOpts.ubsanOpts);

    /* Memory Sanitizer (MSan) */
    if (!hfuzz->enableSanitizers) {
        snprintf(
            hfuzz->sanOpts.msanOpts, sizeof(hfuzz->sanOpts.msanOpts), "MSAN_OPTIONS=" kSAN_REGULAR);
    } else if (hfuzz->useSanCov) {
        snprintf(hfuzz->sanOpts.msanOpts, sizeof(hfuzz->sanOpts.msanOpts),
            "MSAN_OPTIONS=%s:%s:%s:%s%s/%s:%s%s/%s", kMSAN_OPTS, abortFlag, kSAN_COV_OPTS,
            kSANCOVDIR, hfuzz->io.workDir, _HF_SANCOV_DIR, kSANLOGDIR, hfuzz->io.workDir,
            kLOGPREFIX);
    } else {
        snprintf(hfuzz->sanOpts.msanOpts, sizeof(hfuzz->sanOpts.msanOpts),
            "MSAN_OPTIONS=%s:%s:%s%s/%s", kMSAN_OPTS, abortFlag, kSANLOGDIR, hfuzz->io.workDir,
            kLOGPREFIX);
    }
    sanitizers_AddToEnv(hfuzz, "MSAN_OPTIONS=", hfuzz->sanOpts.msanOpts);

    snprintf(
        hfuzz->sanOpts.lsanOpts, sizeof(hfuzz->sanOpts.lsanOpts), "LSAN_OPTIONS=log_threads=1");
    sanitizers_AddToEnv(hfuzz, "LSAN_OPTIONS=", hfuzz->sanOpts.lsanOpts);

    return true;
}
