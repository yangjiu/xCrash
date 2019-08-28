// Copyright (c) 2019-present, iQIYI, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

// Created by caikelun on 2019-08-13.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <android/log.h>
#include "xcc_errno.h"
#include "xcc_util.h"
#include "xcc_signal.h"
#include "xcc_meminfo.h"
#include "xcc_version.h"
#include "xc_anr.h"
#include "xc_common.h"
#include "xc_dl.h"
#include "xc_jni.h"
#include "xc_util.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-statement-expression"

#define XC_ANR_CALLBACK_METHOD_NAME      "anrCallback"
#define XC_ANR_CALLBACK_METHOD_SIGNATURE "(Ljava/lang/String;Ljava/lang/String;)V"

//symbol address in libc++.so and libart.so
static void                            *xc_anr_libcpp_cerr = NULL;
static void                           **xc_anr_libart_runtime_instance = NULL;
static xcc_util_libart_runtime_dump_t   xc_anr_libart_runtime_dump = NULL;

//init parameters
static int                              xc_anr_log_max_count;
static unsigned int                     xc_anr_logcat_system_lines;
static unsigned int                     xc_anr_logcat_events_lines;
static unsigned int                     xc_anr_logcat_main_lines;

//callback
static jmethodID                        xc_anr_cb_method = NULL;
static int                              xc_anr_notifier = -1;

static int xc_anr_load_symbols()
{
    int      r      = XCC_ERRNO_JNI;
    xc_dl_t *libcpp = NULL;
    xc_dl_t *libart = NULL;

    if(NULL !=  xc_anr_libart_runtime_instance &&
       NULL != *xc_anr_libart_runtime_instance &&
       NULL !=  xc_anr_libart_runtime_dump &&
       NULL !=  xc_anr_libcpp_cerr) return 0;

    if(NULL == (libcpp = xc_dl_create(XCC_UTIL_LIBCPP))) goto end;
    if(NULL == (xc_anr_libcpp_cerr = xc_dl_sym(libcpp, XCC_UTIL_LIBCPP_CERR))) goto end;

    if(NULL == (libart = xc_dl_create(XCC_UTIL_LIBART))) goto end;
    if(NULL == (xc_anr_libart_runtime_instance = (void **)xc_dl_sym(libart, XCC_UTIL_LIBART_RUNTIME_INSTANCE))) goto end;
    if(NULL == *xc_anr_libart_runtime_instance) goto end;
    if(NULL == (xc_anr_libart_runtime_dump = (xcc_util_libart_runtime_dump_t)xc_dl_sym(libart, XCC_UTIL_LIBART_RUNTIME_DUMP))) goto end;
    
    r = 0; //OK

 end:
    if(NULL != libcpp) xc_dl_destroy(&libcpp);
    if(NULL != libart) xc_dl_destroy(&libart);
    return r;
}

static int xc_anr_logs_filter(const struct dirent *entry)
{
    size_t len;
    
    if(DT_REG != entry->d_type) return 0;

    len = strlen(entry->d_name);
    if(len < XC_COMMON_LOG_NAME_MIN_ANR) return 0;
    
    if(0 != memcmp(entry->d_name, XC_COMMON_LOG_PREFIX"_", XC_COMMON_LOG_PREFIX_LEN + 1)) return 0;
    if(0 != memcmp(entry->d_name + (len - XC_COMMON_LOG_SUFFIX_ANR_LEN), XC_COMMON_LOG_SUFFIX_ANR, XC_COMMON_LOG_SUFFIX_ANR_LEN)) return 0;

    return 1;
}

static int xc_anr_logs_clean(void)
{
    struct dirent **entry_list;
    int             n, i;
    char            pathname[1024];

    if(0 > (n = scandir(xc_common_log_dir, &entry_list, xc_anr_logs_filter, alphasort))) return XCC_ERRNO_SYS;
    if(n >= xc_anr_log_max_count)
    {
        for(i = 0; i < (n - xc_anr_log_max_count + 1); i++)
        {
            snprintf(pathname, sizeof(pathname), "%s/%s", xc_common_log_dir, entry_list[i]->d_name);
            unlink(pathname);
        }
    }
    free(entry_list);
    return 0;
}

static int xc_anr_write_header(int fd, uint64_t anr_time)
{
    int  r;
    char buf[1024];
    
    xcc_util_get_dump_header(buf, sizeof(buf),
                             XCC_UTIL_CRASH_TYPE_ANR,
                             xc_common_time_zone,
                             xc_common_start_time,
                             anr_time,
                             xc_common_app_id,
                             xc_common_app_version,
                             xc_common_api_level,
                             xc_common_os_version,
                             xc_common_kernel_version,
                             xc_common_abi_list,
                             xc_common_manufacturer,
                             xc_common_brand,
                             xc_common_model,
                             xc_common_build_fingerprint);
    if(0 != (r = xcc_util_write_str(fd, buf))) return r;
    
    if(0 != (r = xcc_util_write_format(fd, "pid: %d  >>> %s <<<\n\n"XCC_UTIL_THREAD_SEP,
                                       xc_common_process_id, xc_common_process_name))) return r;
    
    return 0;
}

static void *xc_anr_dumper(void *arg)
{
    JNIEnv         *env = NULL;
    uint64_t        data;
    uint64_t        anr_time;
    int             fd;
    struct timeval  tv;
    char            pathname[1024];
    jstring         j_pathname;
    
    (void)arg;
    
    pthread_detach(pthread_self());
    pthread_setname_np(pthread_self(), "xcrash_anr_dump");

    if(JNI_OK != (*xc_common_vm)->AttachCurrentThread(xc_common_vm, &env, NULL)) goto exit;

    while(1)
    {
        //block here, waiting for sigquit
        XCC_UTIL_TEMP_FAILURE_RETRY(read(xc_anr_notifier, &data, sizeof(data)));
        
        //ANR time
        if(0 != gettimeofday(&tv, NULL)) break;
        anr_time = (uint64_t)(tv.tv_sec) * 1000 * 1000 + (uint64_t)tv.tv_usec;

        //check if process already crashed
        if(xc_common_crashed) break;

        //check and load symbol address
        if(0 != xc_anr_load_symbols()) break;

        //clean up redundant logs
        //Unlike crash, we can't clean up redundant logs when the APP starts next time.
        if(0 != xc_anr_logs_clean()) continue;

        //create and open log file
        if((fd = xc_common_open_anr_log(pathname, sizeof(pathname), anr_time)) < 0) continue;

        //write header info
        if(0 != xc_anr_write_header(fd, anr_time)) goto end;

        //write ART runtime info
        if(dup2(fd, STDERR_FILENO) < 0) goto end;
        xc_anr_libart_runtime_dump(*xc_anr_libart_runtime_instance, xc_anr_libcpp_cerr);
        dup2(xc_common_fd_null, STDERR_FILENO);

        //write other info
        if(0 != xcc_util_write_str(fd, "\n"XCC_UTIL_THREAD_END"\n")) goto end;
        if(0 != xcc_util_record_logcat(fd, xc_common_process_id, xc_common_api_level, xc_anr_logcat_system_lines, xc_anr_logcat_events_lines, xc_anr_logcat_main_lines)) goto end;
        if(0 != xcc_util_record_fds(fd, xc_common_process_id)) goto end;
        if(0 != xcc_meminfo_record(fd, xc_common_process_id)) goto end;

    end:
        //close log file
        xc_common_close_anr_log(fd);

        //JNI callback
        if(NULL == xc_anr_cb_method) continue;
        if(NULL == (j_pathname = (*env)->NewStringUTF(env, pathname))) continue;
        (*env)->CallStaticVoidMethod(env, xc_common_cb_class, xc_anr_cb_method, j_pathname, NULL);
        XC_JNI_IGNORE_PENDING_EXCEPTION();
        (*env)->DeleteLocalRef(env, j_pathname);
    }
    
    (*xc_common_vm)->DetachCurrentThread(xc_common_vm);

 exit:
    xc_anr_notifier = -1;
    close(xc_anr_notifier);
    return NULL;
}

static void xc_anr_handler(int sig, siginfo_t *si, void *uc)
{
    uint64_t data;
    
    (void)sig;
    (void)si;
    (void)uc;

    if(xc_anr_notifier >= 0)
    {
        data = 1;
        XCC_UTIL_TEMP_FAILURE_RETRY(write(xc_anr_notifier, &data, sizeof(data)));
    }
}

static void xc_anr_init_callback(JNIEnv *env)
{
    if(NULL == xc_common_cb_class) return;
    
    xc_anr_cb_method = (*env)->GetStaticMethodID(env, xc_common_cb_class, XC_ANR_CALLBACK_METHOD_NAME, XC_ANR_CALLBACK_METHOD_SIGNATURE);
    XC_JNI_CHECK_NULL_AND_PENDING_EXCEPTION(xc_anr_cb_method, err);
    return;

 err:
    xc_anr_cb_method = NULL;
}

int xc_anr_init(JNIEnv *env,
                int log_max_count,
                unsigned int logcat_system_lines,
                unsigned int logcat_events_lines,
                unsigned int logcat_main_lines)
{
    int r;
    pthread_t thd;

    //capture ANR only for ART
    if(xc_common_api_level < 21) return 0;

    xc_anr_log_max_count = log_max_count;
    xc_anr_logcat_system_lines = logcat_system_lines;
    xc_anr_logcat_events_lines = logcat_events_lines;
    xc_anr_logcat_main_lines = logcat_main_lines;

    //init for JNI callback
    xc_anr_init_callback(env);

    //create event FD
    if(0 > (xc_anr_notifier = eventfd(0, EFD_CLOEXEC))) return XCC_ERRNO_SYS;

    //register signal handler
    if(0 != (r = xcc_signal_anr_register(xc_anr_handler))) goto err2;

    //create thread for dump ANR info
    if(0 != (r = pthread_create(&thd, NULL, xc_anr_dumper, NULL))) goto err1;

    return 0;

 err1:
    xcc_signal_anr_unregister();
 err2:
    close(xc_anr_notifier);
    xc_anr_notifier = -1;
    
    return r;
}

#pragma clang diagnostic pop
