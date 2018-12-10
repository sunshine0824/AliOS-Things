/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include "ota_hal_os.h"
#include "ota_log.h"

#if (OTA_SIGNAL_CHANNEL) == 1
#include "iot_export.h"
#include "iot_export_coap.h"
#endif
#if (OTA_SIGNAL_CHANNEL) == 2
#include "iot_import.h"
#endif

#ifdef OTA_LINUX
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/reboot.h>
#else
#include <aos/aos.h>
#include <aos/yloop.h>
#include <hal/hal.h>
#endif

/*Memory realloc*/
void *ota_realloc(void *ptr, int size)
{
#if defined OTA_WITH_LINKKIT
    return HAL_Realloc(ptr, size);
#elif !defined OTA_LINUX
    return aos_realloc(ptr, size);
#else
    return realloc(ptr, size);
#endif
}

/*Memory calloc*/
void *ota_calloc(int n, int size)
{
#if defined OTA_WITH_LINKKIT
    return HAL_Malloc(n*size);
#elif !defined OTA_LINUX
    return aos_calloc(n, size);
#else
    return calloc(n, size);
#endif
}

/*Reboot*/
void ota_reboot(void)
{
#if defined OTA_WITH_LINKKIT
    HAL_Reboot();
#elif !defined OTA_LINUX
    aos_reboot();
#else
    reboot(0x1234567);
#endif
}

/*Memory malloc*/
void *ota_malloc(int size)
{
#if defined OTA_WITH_LINKKIT
    return HAL_Malloc(size);
#elif !defined OTA_LINUX
    return aos_malloc(size);
#else
    return malloc(size);
#endif
}

/*Memory free*/
void ota_free(void *ptr)
{
#if defined OTA_WITH_LINKKIT
    return HAL_Free(ptr);
#elif !defined OTA_LINUX
    aos_free(ptr);
#else
    free(ptr);
#endif
}

/*Semaphore init*/
void *ota_semaphore_create(void)
{
#if defined OTA_WITH_LINKKIT
    return HAL_SemaphoreCreate();
#elif !defined OTA_LINUX
    aos_sem_t *sem = (aos_sem_t *)ota_malloc(sizeof(aos_sem_t));
    if (NULL == sem) {
        return NULL;
    }

    if (0 != aos_sem_new(sem, 0)) {
        ota_free(sem);
        return NULL;
    }
    return sem;
#else
    sem_t *sem = (sem_t *)ota_malloc(sizeof(sem_t));
    if (NULL == sem) {
        return NULL;
    }

    if (0 != sem_init(sem, 0, 0)) {
        ota_free(sem);
        return NULL;
    }
    return sem;
#endif
}

/*Semaphore wait*/
int ota_semaphore_wait(void *sem, int ms)
{
#if defined OTA_WITH_LINKKIT
    return HAL_SemaphoreWait(sem, ms);
#elif !defined OTA_LINUX
    return aos_sem_wait((aos_sem_t *)sem, ms);
#else
    if ((~0) == ms) {
        sem_wait(sem);
        return 0;
    } else {
        struct timespec ts;
        int             s;
        /* Restart if interrupted by handler */
        do {
            if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
                return -1;
            }
            s = 0;
            ts.tv_nsec += (ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_nsec -= 1000000000;
                s = 1;
            }
            ts.tv_sec += ms / 1000 + s;
        } while (((s = sem_timedwait(sem, &ts)) != 0) && errno == EINTR);
        return (s == 0) ? 0 : -1;
    }
    return 0;
#endif
}

/*Semaphore post*/
void ota_semaphore_post(void *sem)
{
#if defined OTA_WITH_LINKKIT
    return HAL_SemaphorePost(sem);
#elif !defined OTA_LINUX
    aos_sem_signal((aos_sem_t *)sem);
#else
    sem_post((sem_t *)sem);
#endif
}

/*Semaphore destroy*/
void ota_semaphore_destroy(void *sem)
{
#if defined OTA_WITH_LINKKIT
    return HAL_SemaphoreDestroy(sem);
#elif !defined OTA_LINUX
    aos_sem_free((aos_sem_t *)sem);
    aos_free(sem);
#else
    sem_destroy((sem_t *)sem);
    free(sem);
#endif
}

/*Sleep ms*/
void ota_msleep(int ms)
{
#if defined OTA_WITH_LINKKIT
    return HAL_SleepMs(ms);
#elif !defined OTA_LINUX
    aos_msleep(ms);
#else
    usleep(1000 * ms);
#endif
}

#if defined OTA_WITH_LINKKIT
    ;
#elif !defined OTA_LINUX
typedef struct
{
    aos_task_t task;
    int       detached;
    void *     arg;
    void *(*routine)(void *arg);
} task_context_t;

static void task_wrapper(void *arg)
{
    task_context_t *task = arg;

    task->routine(task->arg);

    if (task) {
        aos_free(task);
        task = NULL;
    }
}
#endif
#define OTA_THREAD_NAME "OTA_Thread"
#define OTA_THREAD_SIZE 4096
#define OTA_THREAD_PRI 30
/*Thread create*/
int ota_thread_create(void **thread_handle, void *(*work_routine)(void *), void *arg, void *pm, int *stack_used)
{
    int ret = -1;
#if defined OTA_WITH_LINKKIT
    ret = HAL_ThreadCreate(thread_handle, work_routine, arg, NULL, 0);
#elif !defined OTA_LINUX
    char * tname = OTA_THREAD_NAME;
    int    ssize = OTA_THREAD_SIZE;
    task_context_t *task = aos_malloc(sizeof(task_context_t));
    if (!task) {
        return -1;
    }
    memset(task, 0, sizeof(task_context_t));
    task->arg      = arg;
    task->routine  = work_routine;

    ret = aos_task_new_ext(&task->task, tname, task_wrapper, task, ssize,OTA_THREAD_PRI);
    *thread_handle = (void *)task;
#else
    ret = pthread_create((pthread_t *)thread_handle, NULL, work_routine, arg);
#endif
    return ret;
}

/*Thread exit*/
void ota_thread_exit(void *thread)
{
#if defined OTA_WITH_LINKKIT
    return HAL_ThreadDelete(thread);
#elif !defined OTA_LINUX
    aos_task_exit(0);
#else
    pthread_exit(0);
#endif
}

#if defined OTA_WITH_LINKKIT
/*KV set*/
int ota_kv_set(const char *key, const void *val, int len, int sync)
{
    return HAL_Kv_Set(key, val, len, sync);
}
/*KV get*/
int ota_kv_get(const char *key, void *buffer, int *len)
{
    return HAL_Kv_Get(key, buffer, len);
}
#elif !defined OTA_LINUX
/*KV set*/
int ota_kv_set(const char *key, const void *val, int len, int sync)
{
    return aos_kv_set(key, val, len, sync);
}

/*KV get*/
int ota_kv_get(const char *key, void *buffer, int *len)
{
    return aos_kv_get(key, buffer, len);
}
#else
#define KV_FILE_PATH "./uota.kv"
#define ITEM_MAX_KEY_LEN 128
#define ITEM_MAX_VAL_LEN 256
#define ITEM_LEN 512

typedef struct
{
    int flag;
    int val_len;
} kv_state_t;

typedef struct
{
    char key[ITEM_MAX_KEY_LEN];
    char val[ITEM_MAX_VAL_LEN];
    kv_state_t state;
} kv_t;

static pthread_mutex_t mutex_kv = PTHREAD_MUTEX_INITIALIZER;
/* get file size and item num */
static int hal_fopen(FILE **fp, int *size, int *num)
{
    /* create an file to save the kv */
    if ((*fp = fopen(KV_FILE_PATH, "a+")) == NULL) {
        OTA_LOG_E("fopen(create) %s error:%s\n", KV_FILE_PATH, strerror(errno));
        return -1;
    }
    fseek(*fp, 0L, SEEK_END);
    OTA_LOG_I("ftell:%d\n", (int)ftell(*fp));
    if ((*size = ftell(*fp)) % ITEM_LEN) {
        OTA_LOG_E("%s is not an kv file\n", KV_FILE_PATH);
        fclose(*fp);
        return -1;
    }
    *num = ftell(*fp) / ITEM_LEN;
    fseek(*fp, 0L, SEEK_SET);
    OTA_LOG_I("file size:%d, block num:%d\n", *size, *num);
    return 0;
}
/*KV set*/
int ota_kv_set(const char *key, const void *val, int len, int sync)
{
    FILE *fp = NULL;
    int file_size = 0, block_num = 0, ret = 0, cur_pos = 0;
    kv_t kv_item;
    int i;
    /* check parameter */
    if (key == NULL || val == NULL) {
        return -1;
    }
    pthread_mutex_lock(&mutex_kv);
    if (hal_fopen(&fp, &file_size, &block_num) != 0) {
        goto ERR;
    }
    for (i = 0; i < block_num; i++) {
        memset(&kv_item, 0, sizeof(kv_t));
        cur_pos = ftell(fp);
        /* read an kv item(512 bytes) from file */
        if ((ret = fread(&kv_item, 1, ITEM_LEN, fp)) != ITEM_LEN) {
            goto ERR;
        }
        /* key compared */
        if (strcmp(kv_item.key, key) == 0) {
            OTA_LOG_I("HAL_Kv_Set@key compared:%s\n", key);
            /* set value and write to file */
            memset(kv_item.val, 0, ITEM_MAX_VAL_LEN);
            memcpy(kv_item.val, val, len);
            kv_item.state.val_len = len;
            fseek(fp, cur_pos, SEEK_SET);
            fwrite(&kv_item, 1, ITEM_LEN, fp);
            goto END;
        }
    }

    OTA_LOG_I("HAL_Kv_Set key:%s\n", key);
    /* key not compared, append an kv to file */
    memset(&kv_item, 0, sizeof(kv_t));
    strcpy(kv_item.key, key);
    memcpy(kv_item.val, val, len);
    kv_item.state.val_len = len;
    fseek(fp, 0L, SEEK_END);
    fwrite(&kv_item, 1, ITEM_LEN, fp);
    goto END;
ERR:
    if (fp == NULL) {
        pthread_mutex_unlock(&mutex_kv);
        return -1;
    }
    OTA_LOG_E("read %s error:%s\n", KV_FILE_PATH, strerror(errno));
    fflush(fp);
    fclose(fp);
    pthread_mutex_unlock(&mutex_kv);

    return -1;
END:
    fflush(fp);
    fclose(fp);
    pthread_mutex_unlock(&mutex_kv);
    return 0;
}

/*KV get*/
int ota_kv_get(const char *key, void *buffer, int *len)
{
    FILE *fp = NULL;
    int i;
    /* read from file */
    int file_size = 0, block_num = 0;
    kv_t kv_item;
    /* check parameter */
    if (key == NULL || buffer == NULL || len == NULL) {
        return -1;
    }
    pthread_mutex_lock(&mutex_kv);
    if (hal_fopen(&fp, &file_size, &block_num) != 0) {
        goto ERR;
    }
    for (i = 0; i < block_num; i++) {
        memset(&kv_item, 0, sizeof(kv_t));
        /* read an kv item(512 bytes) from file */
        if (fread(&kv_item, 1, ITEM_LEN, fp) != ITEM_LEN) {
            goto ERR;
        }
        /* key compared */
        if (strcmp(kv_item.key, key) == 0) {
            OTA_LOG_I("HAL_Kv_Get@key compared:%s\n", key);
            /* set value and write to file */
            *len = kv_item.state.val_len;
            memcpy(buffer, kv_item.val, *len);
            goto END;
        }
    }
    OTA_LOG_I("can not find the key:%s\n", key);
    goto END;
ERR:
    if (fp == NULL) {
        pthread_mutex_unlock(&mutex_kv);
        return -1;
    }
    OTA_LOG_E("read %s error:%s\n", KV_FILE_PATH, strerror(errno));
    fflush(fp);
    fclose(fp);
    pthread_mutex_unlock(&mutex_kv);
    return -1;
END:
    fflush(fp);
    fclose(fp);
    pthread_mutex_unlock(&mutex_kv);
    return 0;
}

/*Timer create*/
void *ota_timer_create(const char *name, void (*func)(void *), void *user_data)
{
    timer_t *timer = NULL;
    struct sigevent ent;
    /* check parameter */
    if (func == NULL)
        return NULL;
    timer = (timer_t *)malloc(sizeof(time_t));
    /* Init */
    memset(&ent, 0x00, sizeof(struct sigevent));
    /* create a timer */
    ent.sigev_notify = SIGEV_THREAD;
    ent.sigev_notify_function = (void (*)(union sigval))func;
    ent.sigev_value.sival_ptr = user_data;
    if (timer_create(CLOCK_MONOTONIC, &ent, timer) != 0) {
        OTA_LOG_E("timer_create");
        return NULL;
    }
    return (void *)timer;
}

/*Timer start*/
int ota_timer_start(void *timer, int ms)
{
    struct itimerspec ts;
    /* check parameter */
    if (timer == NULL)
        return -1;
    /* it_interval=0: timer run only once */
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;

    /* it_value=0: stop timer */
    ts.it_value.tv_sec = ms / 1000;
    ts.it_value.tv_nsec = (ms % 1000) * 1000;

    return timer_settime(*(timer_t *)timer, 0, &ts, NULL);
}
#endif /*Linux end*/

/*Socket API*/
void* ota_socket_connect(char *host, int port)
{
   #ifdef OTA_WITH_LINKKIT
   return (void*)HAL_TCP_Establish(host, port);
   #else
   return 0;
   #endif
}

int ota_socket_send(void* fd, char *buf, int len)
{
   #ifdef OTA_WITH_LINKKIT
   return HAL_TCP_Write((uintptr_t)fd, buf, len, OTA_SSL_TIMEOUT);
   #else
   return 0;
   #endif
}

int ota_socket_recv(void* fd, char *buf, int len)
{
   #ifdef OTA_WITH_LINKKIT
   return HAL_TCP_Read((uintptr_t)fd, buf, len, OTA_SSL_TIMEOUT);
   #else
   return 0;
   #endif
}

void ota_socket_close(void* fd)
{
   #ifdef OTA_WITH_LINKKIT
   HAL_TCP_Destroy((uintptr_t)fd);
   #endif
}

/*SSL connect*/
void *ota_ssl_connect(const char *host, unsigned short port, const char *ca_crt, int ca_crt_len)
{
    #ifdef OTA_WITH_LINKKIT
    return (void*)HAL_SSL_Establish(host, port, ca_crt, ca_crt_len);
    #else
    return 0;
    #endif
}

/*SSL send*/
int ota_ssl_send(void *ssl, char *buf, int len)
{
    #ifdef OTA_WITH_LINKKIT
    return HAL_SSL_Write((uintptr_t)ssl, buf, len, OTA_SSL_TIMEOUT);
    #else
    return 0;
    #endif
}

/*SSL recv*/
int ota_ssl_recv(void *ssl, char *buf, int len)
{
    #ifdef OTA_WITH_LINKKIT
    return HAL_SSL_Read((uintptr_t)ssl, buf, len, OTA_SSL_TIMEOUT);
    #else
    return 0;
    #endif
}

/*base64*/
static const unsigned char base64_dec_map[128] =
{
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127,  62, 127, 127, 127,  63,  52,  53,
     54,  55,  56,  57,  58,  59,  60,  61, 127, 127,
    127,  64, 127, 127, 127,   0,   1,   2,   3,   4,
      5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
     15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
     25, 127, 127, 127, 127, 127, 127,  26,  27,  28,
     29,  30,  31,  32,  33,  34,  35,  36,  37,  38,
     39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
     49,  50,  51, 127, 127, 127, 127, 127
};

int ota_base64_decode( unsigned char *dst, unsigned int *dlen,
                   const unsigned char *src, unsigned int slen )
{
    unsigned int i, n;
    unsigned int j, x;
    unsigned char *p;

    for( i = n = j = 0; i < slen; i++ )
    {
        if( ( slen - i ) >= 2 &&
            src[i] == '\r' && src[i + 1] == '\n' )
            continue;

        if( src[i] == '\n' )
            continue;

        if( src[i] == '=' && ++j > 2 )
            return -1;

        if( src[i] > 127 || base64_dec_map[src[i]] == 127 )
            return -1;

        if( base64_dec_map[src[i]] < 64 && j != 0 )
            return -1;

        n++;
    }

    if( n == 0 )
        return 0;

    n = ( ( n * 6 ) + 7 ) >> 3;
    n -= j;

    if( dst == 0 || *dlen < n )
    {
        *dlen = n;
        return -2;
    }

   for( j = 3, n = x = 0, p = dst; i > 0; i--, src++ )
   {
        if( *src == '\r' || *src == '\n' )
            continue;

        j -= ( base64_dec_map[*src] == 64 );
        x  = ( x << 6 ) | ( base64_dec_map[*src] & 0x3F );

        if( ++n == 4 )
        {
            n = 0;
            if( j > 0 ) *p++ = (unsigned char)( x >> 16 );
            if( j > 1 ) *p++ = (unsigned char)( x >>  8 );
            if( j > 2 ) *p++ = (unsigned char)( x       );
        }
    }

    *dlen = p - dst;

    return 0;
}

/*MQTT API*/
int ota_hal_mqtt_publish(char *topic, int qos, void *data, int len)
{
    #if (OTA_SIGNAL_CHANNEL) == 1
    return IOT_MQTT_Publish_Simple(NULL, topic, qos, data, len);
    #else
    return 0;
    #endif
}

int ota_hal_mqtt_subscribe(char *topic, void *cb, void *ctx)
{
    #if (OTA_SIGNAL_CHANNEL) == 1
    return IOT_MQTT_Subscribe_Sync(NULL, topic, 0, cb, ctx, 1000);
    #else
    return 0;
    #endif
}

int ota_hal_mqtt_deinit(void)
{
    #if (OTA_SIGNAL_CHANNEL) == 1
    return IOT_MQTT_Destroy(NULL);
    #else
    return 0;
    #endif
}

int ota_hal_mqtt_init(void)
{
    #if (OTA_SIGNAL_CHANNEL) == 1
    return (IOT_MQTT_Construct(NULL) == NULL)? -1 : 0;
    #else
    return 0;
    #endif
}

/*CoAP API*/
int ota_coap_send(void *p_context, char *p_path, void *p_message)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    return IOT_CoAP_SendMessage(p_context, p_path, p_message);
    #else
    return 0;
    #endif
}

int ota_coap_parse_block(void *p_message, int type, int *num, int *more, int *size)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    return IOT_CoAP_ParseOption_block(p_message, type, num, more, size);
    #else
    return 0;
    #endif
}

int ota_coap_send_block(void *p_context, char *p_path, void *p_message, int block_type, int num, int more, int size)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    return IOT_CoAP_SendMessage_block(p_context, p_path, p_message, block_type,num, more, size);
    #else
    return 0;
    #endif
}
int ota_coap_get_payload(void *p_message, const char **pp_payload, int *p_len)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    return IOT_CoAP_GetMessagePayload(p_message, pp_payload, p_len);
    #else
    return 0;
    #endif
}
int ota_coap_get_code(void *p_message, void *p_resp_code)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    return IOT_CoAP_GetMessageCode(p_message, p_resp_code);
    #else
    return 0;
    #endif
}
int ota_coap_init(void)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    #define COAP_ONLINE_DTLS_SERVER_URL "coaps://%s.iot-as-coap.cn-shanghai.aliyuncs.com:5684"
    int ret = 0;
    iotx_coap_config_t   config;
    iotx_device_info_t   dev;
    memset(&config, 0, sizeof(config));
    memset(&dev, 0, sizeof(dev));
    strncpy(dev.device_id, get_ota_service()->ps, sizeof(dev.device_id)-1);
    strncpy(dev.product_key, get_ota_service()->pk, sizeof(dev.product_key)-1);
    strncpy(dev.device_name, get_ota_service()->dn, sizeof(dev.device_name)-1);
    strncpy(dev.device_secret, get_ota_service()->ds, sizeof(dev.device_secret)-1);
    config.p_devinfo = &dev;
    char url[256] = { 0 };
    ota_snprintf(url, sizeof(url), COAP_ONLINE_DTLS_SERVER_URL,get_ota_service()->pk);
    config.p_url = url;
    get_ota_service()->h_ch = (void *)ota_IOT_CoAP_Init(&config);
    if (get_ota_service()->h_ch) {
        ret = ota_IOT_CoAP_DeviceNameAuth(get_ota_service()->h_ch);
        if (ret < 0) {
            OTA_LOG_E("COAP error");
            return ret;
        }
        OTA_LOG_D("IOT_CoAP_DeviceNameAuth. success.");
    }
    #else
    return 0;
    #endif
}
int ota_coap_deinit(void)
{
    #if (OTA_SIGNAL_CHANNEL) == 2
    IOT_CoAP_Deinit(get_ota_service()->h_ch);
    #endif
    return 0;
}