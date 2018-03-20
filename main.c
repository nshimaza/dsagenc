/*
 *
 * Copyright (c) 2016,2018 Cisco and/or its affiliates.
 *
 * This software is licensed to you under the terms of the Cisco Sample
 * Code License, Version 1.0 (the "License"). You may obtain a copy of the
 * License at
 *
 *                https://developer.cisco.com/docs/licenses
 *
 * All use of the material herein must be in accordance with the terms of
 * the License. All rights not expressly granted by the License are
 * reserved. Unless required by applicable law or agreed to separately in
 * writing, software distributed under the License is distributed on an "AS
 * IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "main"

#include <dslink/dslink.h>
#include <dslink/log.h>
#include <dslink/storage/storage.h>

typedef struct DSLinkParams {
    int             argc;
    char**          argv;
    const char*     linkName;
    int             isRequester;
    int             isResponder;
    DSLinkCallbacks cbs;
} DSLinkParams;


#define SetValueQueue_Length 100

typedef struct SetValueQueue {
    pthread_mutex_t mutex;
    json_t**        head;
    json_t**        tail;
    json_t*         ring[SetValueQueue_Length];
} SetValueQueue;


typedef struct UpdaterParams {
    int             boost;
    int             interval;
    int             agingCount;
    int             stopAt;
    SetValueQueue*  queue;
    uv_async_t*     asyncHandle;
    DSLink* link;
    DSNode* node;
} UpdaterParams;


static int boost = 1;
static int interval = 10;
static int agingCount = 6000;
static int startCount = 12000;
static int stopAt = 60000;
static int count = 0;



struct DsaGenC {
    DSLink*         link;
    DSNode*         node;
    bool            isInitialized;
    uv_async_t      asyncHandle;
    SetValueQueue   queue;
} dsagenc;


void SetValueQueue_init(SetValueQueue* queue) {
    pthread_mutex_init(&queue->mutex, NULL);
    queue->head = queue->tail = queue->ring;
}

void SetValueQueue_enqueue(SetValueQueue* queue, json_t* value) {
    pthread_mutex_lock(&queue->mutex);
    json_t** next_head = queue->head + 1 < queue->ring + SetValueQueue_Length ? queue->head + 1 : queue->ring;
    if (next_head == queue->tail) {
        /* no more available space in the ring buffer.  discard value silently */
        json_decref(value);
    } else {
        *next_head = value;
        queue->head = next_head;
    }
    pthread_mutex_unlock(&queue->mutex);
}

json_t* SetValueQueue_dequeue(SetValueQueue* queue) {
    pthread_mutex_lock(&queue->mutex);
    json_t* result = NULL;
    if (queue->tail != queue->head) {
        result = *queue->tail;
        queue->tail = queue->tail + 1 < queue->ring + SetValueQueue_Length ? queue->tail + 1 : queue->ring;
    }
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

void timespec_add(struct timespec* result, time_t tv_sec, long tv_nsec, long offset) {
    long next_nsec = tv_nsec + offset;
    result->tv_nsec = next_nsec % 1000000000;
    result->tv_sec = tv_sec + (next_nsec / 1000000000);
}

void updater_set_value(UpdaterParams* params, json_t* value) {
    SetValueQueue_enqueue(params->queue, value);
    uv_async_send(params->asyncHandle);
}

void* thread_updater(void* thData) {
    UpdaterParams* p = thData;
    int count = 0;
    int startCount = p->agingCount * 2;

    {
        struct timespec req = { 10, 0 };
        nanosleep(&req, NULL);
    }

    int currCount = count * p->boost;
    while (currCount <= p->stopAt) {

        {
            struct timespec req = { 0, p->interval * 1000000 };
            nanosleep(&req, NULL);
        }

        if (currCount < p->agingCount) {
            for (int i = 0; i < p->boost; i++) {
                updater_set_value(p, json_integer(currCount + i));
            }
        } else if (currCount < startCount) {
            /* do nothing */
        } else if (currCount < p->stopAt) {
            for (int i = 0; i < p->boost; i++) {
                updater_set_value(p, json_integer(currCount + i));
            }
        } else if (currCount == p->stopAt) {
            updater_set_value(p, json_integer(-1));
        }

        count++;
        currCount = count * p->boost;
    }

    return NULL;
}







void terminator(uv_timer_t* timer) {
    (void) timer;
    printf("finished\n");
    exit(0);
}


void set_counter(uv_timer_t* timer) {
    void** data = timer->data;
    DSLink* link = data[0];
    DSNode* node = data[1];

    link = dsagenc.link;
    node = dsagenc.node;

//    if (!dslink_map_contains(link->responder->value_path_subs, (void*) node->path)) {
//        dslink_free(data);
//        uv_timer_stop(timer);
//        return;
//    }

    int currCount = count * boost;
    count++;

    if (currCount < agingCount) {
        for (int i = 0; i < boost; i++) {
            dslink_node_set_value(link, node, json_integer(currCount + i));
        }
    } else if (currCount < startCount) {
        /* do nothing */
    } else if (currCount < stopAt) {
        for (int i = 0; i < boost; i++) {
            dslink_node_set_value(link, node, json_integer(currCount + i));
        }
    } else if (currCount == stopAt) {
        dslink_node_set_value(link, node, json_integer(-1));
    } else {
        dslink_free(data);
        uv_timer_stop(timer);

        uv_timer_t* termTimer = malloc(sizeof(uv_timer_t));
        uv_timer_init(&link->loop, termTimer);
        uv_timer_start(timer, terminator, 1000, 0);
    }
}






void async_set_value_cb(uv_async_t* handle) {
    struct DsaGenC* p = handle->data;
    json_t* value;
    while ((value = SetValueQueue_dequeue(&p->queue)) != NULL) {
            dslink_node_set_value(p->link, p->node, value);
    }
}

// Called to initialize your node structure.
void on_init(DSLink* link) {
    json_t *messageValue = dslink_json_get_config(link, "message");
    if (messageValue) {
        log_info("Message = %s\n", json_string_value(messageValue));
    }

    DSNode* superRoot = link->responder->super_root;

    DSNode* num = dslink_node_create(superRoot, "c1", "node");
    if (!num) {
        log_warn("Failed to create the cX node\n");
        return;
    }

    if (dslink_node_set_meta(link, num, "$type", json_string("number")) != 0) {
        log_warn("Failed to set the type on the cX\n");
        dslink_node_tree_free(link, num);
        return;
    }

    if (dslink_node_set_value(link, num, json_integer(-2)) != 0) {
        log_warn("Failed to set the value on the cX\n");
        dslink_node_tree_free(link, num);
        return;
    }

    if (dslink_node_add_child(link, num) != 0) {
        log_warn("Failed to add the cX node to the root\n");
        dslink_node_tree_free(link, num);
    }

    dsagenc.link = link;
    dsagenc.node = num;
    uv_async_init(&link->loop, &dsagenc.asyncHandle, async_set_value_cb);
    dsagenc.asyncHandle.data = &dsagenc;
    dsagenc.isInitialized = true;

    void** timerData = malloc(sizeof(void*) * 2);
    timerData[0] = link;
    timerData[1] = num;

    uv_timer_t* timer = malloc(sizeof(uv_timer_t));
    uv_timer_init(&link->loop, timer);
    timer->data = timerData;
    uv_timer_start(timer, set_counter, 10000, interval);

    // add link data
    json_t* linkData = json_object();
    json_object_set_nocheck(linkData, "test", json_true());
    link->linkData = linkData;

    log_info("Initialized!\n");
}

// Called when the DSLink is connected.
void on_connected(DSLink* link) {
    (void) link;
    log_info("Connected!\n");
}

// Called when the DSLink is disconnected.
// If this was not initiated by dslink_close,
// then a reconnection attempt is made.
void on_disconnected(DSLink* link) {
    (void) link;
    log_info("Disconnected!\n");
}

void* thread_dslink(void* thData) {
    DSLinkParams* p = thData;

    dslink_init(p->argc, p->argv, p->linkName, p->isRequester, p->isResponder, &p->cbs);

    return NULL;
}

void start_dslink(DSLinkParams* params) {
    pthread_t thread;

    if (pthread_create(&thread, NULL, thread_dslink, params) != 0) {
        log_warn("Failed to create a new thread for DSLink\n");
    }
}


int main(int argc, char* argv[])
{
    dsagenc.link = NULL;
    dsagenc.node = NULL;
    dsagenc.isInitialized = false;
    SetValueQueue_init(&dsagenc.queue);

    boost = atoi(argv[2]);
    int schedPerSec = atoi(argv[3]);
    interval = 1000 / schedPerSec;
    agingCount = schedPerSec * 60 * boost;
    startCount = agingCount * 2;
    stopAt = schedPerSec * atoi(argv[4]) * boost + startCount;
    char linkName[256];
    sprintf(linkName, "dsagenc%d", atoi(argv[1]));
    char* dslink_argv[] = { argv[0], argv[5] };



    DSLinkParams params = {
        argc - 4,
        dslink_argv,
        linkName,
        0, /* isRequester */
        1, /* isResponder */
        {
            on_init,
            on_connected,
            on_disconnected,
            NULL
        } /* DSLinkCallbacks */
    };

    start_dslink(&params);

    for (;;) {
        sleep(1000);
    }

    return 0;
}
