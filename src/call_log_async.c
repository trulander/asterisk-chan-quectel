#include "call_log_async.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Параметры одного сохранённого вызова копируются в событие целиком, без borrowed pointers. */
typedef struct call_log_event {
    int type; /**< START, ANSWER, SET_INDEX или FINISH. */
    sqlite3_int64 id; /**< Зарезервированный rowid, одновременно durable call-<id>. */
    int call_idx; /**< Текущий modem index; -1 означает, что modem index пока неизвестен. */
    int direction; /**< CALL_LOG_INCOMING/OUTGOING; применяется только к START. */
    int answered; /**< Запрошенный hangup status; очередь также содержит prior ANSWER. */
    char device[CALL_LOG_ASYNC_DEVICE_LEN]; /**< Копия SIM/device для START. */
    char number[CALL_LOG_ASYNC_NUMBER_LEN]; /**< Копия номера для START. */
    char timestamp[CALL_LOG_ASYNC_TIME_LEN]; /**< Копия UTC времени события. */
} call_log_event_t;

typedef struct call_log_active {
    int used; /**< Запись доступна для correlation до успешного SQLite FINISH commit. */
    int finishing; /**< FINISH уже в FIFO: не дедуплицировать с новым вызовом того же modem index. */
    sqlite3_int64 id; /**< ID поколения; сравнивается вместе с device/index. */
    int call_idx; /**< Текущий переиспользуемый modem index. */
    char device[CALL_LOG_ASYNC_DEVICE_LEN]; /**< SIM identity для no-channel release lookup. */
} call_log_active_t;

struct call_log_async {
    sqlite3* db; /**< smsdb connection; writer единственный call-history mutator. */
    pthread_t thread; /**< Постоянный последовательный DB writer. */
    pthread_mutex_t lock; /**< Защищает ring, ID allocator и active map; producer берёт trylock. */
    pthread_cond_t ready; /**< Разбудить writer после enqueue/stop. */
    pthread_cond_t drained; /**< Разбудить flush после завершения события. */
    int stopping; /**< После установки writer дописывает ring и завершает цикл. */
    int head; /**< Индекс первого ожидающего события. */
    int count; /**< Число элементов в ring; предел задан capacity. */
    int in_flight; /**< События, уже вынутые worker-ом и выполняемые в SQLite. */
    sqlite3_int64 next_id; /**< Следующий rowid, зарезервированный только в памяти. */
    call_log_event_t events[CALL_LOG_ASYNC_QUEUE_CAPACITY]; /**< Fixed-size FIFO без per-event malloc. */
    call_log_active_t active[CALL_LOG_ASYNC_QUEUE_CAPACITY]; /**< Активные incoming поколения для дедупа/lookup. */
};

/* Типы FIFO-команд; все они адресуют строку по зарезервированному stable rowid. */
enum { CALL_LOG_EVENT_START = 1, CALL_LOG_EVENT_ANSWER, CALL_LOG_EVENT_SET_INDEX, CALL_LOG_EVENT_FINISH };

/** Скопировать короткую строку в событие и отклонить усечение, чтобы ID/device не спутались. */
static int copy_field(char* dst, size_t len, const char* src)
{
    if (!src) src = "";
    const size_t n = strlen(src);
    if (n >= len) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

/** Добавить готовое событие под lock; память ring принадлежит queue и не выделяется. */
static int push_locked(call_log_async_t* q, const call_log_event_t* event)
{
    if (q->stopping || q->count == CALL_LOG_ASYNC_QUEUE_CAPACITY) return -1;
    const int tail = (q->head + q->count) % CALL_LOG_ASYNC_QUEUE_CAPACITY;
    q->events[tail] = *event;
    ++q->count;
    pthread_cond_signal(&q->ready);
    return 0;
}

/** Выполнить одно событие через существующие SQLite call_log transactions на writer thread. */
static int execute_event(call_log_async_t* q, const call_log_event_t* e)
{
    char id[CALL_LOG_ID_LEN];
    if (snprintf(id, sizeof(id), "call-%lld", (long long)e->id) >= (int)sizeof(id)) return -1;
    switch (e->type) {
        case CALL_LOG_EVENT_START:
            return call_log_start_reserved(q->db, e->id, e->device, e->call_idx,
                                           (call_log_direction_t)e->direction, e->number, e->timestamp);
        case CALL_LOG_EVENT_ANSWER:
            return call_log_answer(q->db, id);
        case CALL_LOG_EVENT_SET_INDEX:
            return call_log_set_index(q->db, id, e->call_idx);
        case CALL_LOG_EVENT_FINISH:
            return call_log_finish(q->db, id, e->answered, e->timestamp);
        default:
            return -1;
    }
}

/** Один writer сериализует SQLite операции в enqueue-порядке, не удерживая queue lock при I/O. */
static void* writer_main(void* arg)
{
    call_log_async_t* q = arg;
    for (;;) {
        call_log_event_t event;
        pthread_mutex_lock(&q->lock);
        while (!q->count && !q->stopping) pthread_cond_wait(&q->ready, &q->lock);
        if (!q->count && q->stopping) {
            pthread_mutex_unlock(&q->lock);
            return NULL;
        }
        event = q->events[q->head];
        q->head = (q->head + 1) % CALL_LOG_ASYNC_QUEUE_CAPACITY;
        --q->count;
        ++q->in_flight;
        pthread_mutex_unlock(&q->lock);

        const int event_error = execute_event(q, &event);
        if (event_error) {
            /* Ошибка не блокирует producer и не останавливает очередь; последующие
             * события продолжат работать, а исходная потеря видна в журнале stderr. */
            fprintf(stderr, "call_log_async: SQLite event failed (type=%d id=%lld)\n",
                    event.type, (long long)event.id);
        }

        pthread_mutex_lock(&q->lock);
        if (event.type == CALL_LOG_EVENT_START && event_error) {
            /* Не оставлять active-map entry, для которого не удалось создать DB row. */
            for (size_t i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY; ++i) {
                if (q->active[i].used && q->active[i].id == event.id) q->active[i].used = 0;
            }
        } else if (event.type == CALL_LOG_EVENT_FINISH) {
            /* FINISH убирает поколение только после успешного SQLite commit; при ошибке
             * active ID вновь становится кандидатом для retry/release обработки. */
            for (size_t i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY; ++i) {
                if (!q->active[i].used || q->active[i].id != event.id) continue;
                if (event_error) q->active[i].finishing = 0;
                else q->active[i].used = 0;
            }
        }
        --q->in_flight;
        if (!q->count && !q->in_flight) pthread_cond_broadcast(&q->drained);
        pthread_mutex_unlock(&q->lock);
    }
}

/** Выбрать seed выше и фактических rowid, и сохранённого AUTOINCREMENT high-water mark. */
int call_log_async_seed_id(sqlite3* db, sqlite3_int64* next_id)
{
    if (!db || !next_id) return -1;
    sqlite3_stmt* stmt = NULL;
    const char* sql = "SELECT MAX(COALESCE(MAX(id),0), "
                      "COALESCE((SELECT seq FROM sqlite_sequence WHERE name='call_log'),0))+1 "
                      "FROM call_log";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) *next_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW && *next_id > 0 ? 0 : -1;
}

/** Запустить serial writer; queue allocation происходит при инициализации модуля, не на call path. */
call_log_async_t* call_log_async_start(sqlite3* db, sqlite3_int64 next_id)
{
    if (!db || next_id <= 0) return NULL;
    call_log_async_t* q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->db = db;
    q->next_id = next_id;
    if (pthread_mutex_init(&q->lock, NULL)) { free(q); return NULL; }
    if (pthread_cond_init(&q->ready, NULL)) {
        pthread_mutex_destroy(&q->lock);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->drained, NULL)) {
        pthread_cond_destroy(&q->ready);
        pthread_mutex_destroy(&q->lock);
        free(q);
        return NULL;
    }
    if (pthread_create(&q->thread, NULL, writer_main, q)) {
        pthread_cond_destroy(&q->drained);
        pthread_cond_destroy(&q->ready);
        pthread_mutex_destroy(&q->lock);
        free(q);
        return NULL;
    }
    return q;
}

/** Enqueue producer берёт lock неблокирующе и копирует весь payload в bounded FIFO. */
static int producer_lock(call_log_async_t* q)
{
    if (!q) return -1;
    const int rc = pthread_mutex_trylock(&q->lock);
    return rc == 0 ? 0 : -1;
}

/** Сгенерировать call-<rowid> и поставить START, удерживая только короткий queue mutex. */
int call_log_async_start_call(call_log_async_t* q, const char* device, int call_idx,
                              call_log_direction_t direction, const char* number,
                              const char* timestamp, int force_new_generation,
                              char* call_id, size_t call_id_len)
{
    if (!device || !timestamp || !call_id || !call_id_len ||
        (direction != CALL_LOG_INCOMING && direction != CALL_LOG_OUTGOING)) return -1;
    if (producer_lock(q)) return -1;

    if (direction == CALL_LOG_INCOMING && !force_new_generation && call_idx >= 0) {
        sqlite3_int64 newest = 0;
        for (size_t i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY; ++i) {
            call_log_active_t* active = &q->active[i];
            if (active->used && !active->finishing && active->call_idx == call_idx &&
                !strcmp(active->device, device) && active->id > newest) newest = active->id;
        }
        if (newest) {
            const int n = snprintf(call_id, call_id_len, "call-%lld", (long long)newest);
            pthread_mutex_unlock(&q->lock);
            return n >= 0 && (size_t)n < call_id_len ? 0 : -1;
        }
    }

    int active_slot = -1;
    if (direction == CALL_LOG_INCOMING) {
        for (size_t i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY; ++i) {
            if (!q->active[i].used) { active_slot = (int)i; break; }
        }
        if (active_slot < 0) { pthread_mutex_unlock(&q->lock); return -1; }
    }

    call_log_event_t e = {.type = CALL_LOG_EVENT_START, .id = q->next_id,
                          .call_idx = call_idx, .direction = direction};
    if (copy_field(e.device, sizeof(e.device), device) || copy_field(e.number, sizeof(e.number), number) ||
        copy_field(e.timestamp, sizeof(e.timestamp), timestamp)) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    const int n = snprintf(call_id, call_id_len, "call-%lld", (long long)e.id);
    if (n < 0 || (size_t)n >= call_id_len || push_locked(q, &e)) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    ++q->next_id;
    if (direction == CALL_LOG_INCOMING) {
        call_log_active_t* active = &q->active[active_slot];
        active->used = 1;
        active->id = e.id;
        active->call_idx = call_idx;
        copy_field(active->device, sizeof(active->device), device);
    }
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/** Поставить одно короткое событие по устойчивому call ID без SQLite и без ожидания mutex. */
static int enqueue_id_event(call_log_async_t* q, int type, const char* call_id, int value, const char* timestamp)
{
    if (!call_id) return -1;
    long long id = 0;
    char trailing = '\0';
    if (sscanf(call_id, "call-%lld%c", &id, &trailing) != 1 || id <= 0) return -1;
    if (producer_lock(q)) return -1;
    call_log_event_t e = {.type = type, .id = id, .call_idx = value, .answered = value};
    if (type == CALL_LOG_EVENT_FINISH && copy_field(e.timestamp, sizeof(e.timestamp), timestamp)) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    if (push_locked(q, &e)) { pthread_mutex_unlock(&q->lock); return -1; }
    if (type == CALL_LOG_EVENT_FINISH) {
        for (size_t i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY; ++i) {
            if (q->active[i].used && q->active[i].id == id) q->active[i].finishing = 1;
        }
    }
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/** Асинхронно отметить ответ; FIFO гарантирует START перед ANSWER. */
int call_log_async_answer(call_log_async_t* q, const char* call_id)
{
    return enqueue_id_event(q, CALL_LOG_EVENT_ANSWER, call_id, 1, NULL);
}

/** Асинхронно обновить краткоживущий modem index для конкретного ID. */
int call_log_async_set_index(call_log_async_t* q, const char* call_id, int call_idx)
{
    return enqueue_id_event(q, CALL_LOG_EVENT_SET_INDEX, call_id, call_idx, NULL);
}

/** Асинхронно завершить запись; для incoming missed finish+mt_queue остаются одной writer transaction. */
int call_log_async_finish(call_log_async_t* q, const char* call_id, int answered, const char* timestamp)
{
    return enqueue_id_event(q, CALL_LOG_EVENT_FINISH, call_id, answered, timestamp);
}

/** Поиск активного incoming под trylock; busy queue означает быстрый error, не DB fallback. */
int call_log_async_active_id(call_log_async_t* q, const char* device, int call_idx,
                             char* call_id, size_t call_id_len)
{
    if (!device || !call_id || !call_id_len || producer_lock(q)) return -1;
    sqlite3_int64 newest = 0;
    for (size_t i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY; ++i) {
        call_log_active_t* active = &q->active[i];
        if (active->used && active->call_idx == call_idx && !strcmp(active->device, device) && active->id > newest) newest = active->id;
    }
    if (!newest) { pthread_mutex_unlock(&q->lock); return 0; }
    const int n = snprintf(call_id, call_id_len, "call-%lld", (long long)newest);
    pthread_mutex_unlock(&q->lock);
    return n >= 0 && (size_t)n < call_id_len ? 1 : -1;
}

/** Тест/shutdown barrier ждёт и ring, и исполняемое событие; critical producers не вызывают его. */
int call_log_async_flush(call_log_async_t* q)
{
    if (!q) return -1;
    pthread_mutex_lock(&q->lock);
    while (q->count || q->in_flight) pthread_cond_wait(&q->drained, &q->lock);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/** Завершить writer после drain и освободить queue state; вызывается до закрытия SQLite. */
void call_log_async_stop(call_log_async_t** queue)
{
    if (!queue || !*queue) return;
    call_log_async_t* q = *queue;
    pthread_mutex_lock(&q->lock);
    q->stopping = 1;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->lock);
    pthread_join(q->thread, NULL);
    pthread_cond_destroy(&q->drained);
    pthread_cond_destroy(&q->ready);
    pthread_mutex_destroy(&q->lock);
    free(q);
    *queue = NULL;
}
