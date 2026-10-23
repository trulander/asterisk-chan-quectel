#include "sim_metrics.h"
#include "web_sim_metrics.h"
#include "sms_paging.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Подготовить настоящую схему, совпадающую с колонками, которые читает production SQL. */
static void exec_sql(sqlite3 *db, const char *sql)
{
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

/** Сценарий: создать в настоящей SQLite обе production SMS-таблицы и call_log, затем заполнить
 * две SIM разными MT/MO статусами, включая NULL SRR, доставленные строки и активные звонки.
 * Предусловия: production sms_paging_count и sim_metrics_read вызываются на той же схеме,
 * что production использует для счётчиков; значения записаны обычным SQL, без fake-DB.
 * Проверки: SMS total включает обе таблицы, undelivered исключает только подтверждённую доставку,
 * calls_total включает активные вызовы, а SIM2 не получает ни одной строки SIM1.
 * Предотвращаемая регрессия: общее SMS число начинает считать только один поток, фильтр
 * теряется/становится неточным, delivered учитывается как pending, либо активные звонки пропадают.
 */
static void test_mapping_and_isolation(void)
{
    sqlite3 *db = NULL;
    struct sim_metrics a, b;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    exec_sql(db, "CREATE TABLE mt_queue(device TEXT, mid TEXT, from_num TEXT, msg TEXT, ts TEXT, state TEXT, created_at INTEGER, attempts INTEGER, next_retry_at INTEGER, last_result TEXT, sent_at INTEGER, delivered_at INTEGER);");
    exec_sql(db, "CREATE TABLE mt_out(id INTEGER, device TEXT, dst TEXT, msg TEXT, ts TEXT, state TEXT, srr_status INTEGER, mr INTEGER, created_at INTEGER, updated_at INTEGER);");
    exec_sql(db, "CREATE TABLE call_log(device TEXT, finished INTEGER);");
    exec_sql(db, "INSERT INTO mt_queue(device,mid,from_num,msg,state,created_at) VALUES('sim1','a','x','m','new',1),('sim1','b','x','m','retrying',2),('sim1','c','x','m','delivered',3),('sim2','d','x','m','new',4);");
    exec_sql(db, "INSERT INTO mt_out(id,device,dst,msg,state,srr_status,created_at) VALUES(1,'sim1','x','m','submitted_to_modem',NULL,5),(2,'sim1','x','m','smsc_ok',0,6),(3,'sim1','x','m','failed',64,7),(4,'sim1','x','m','delivered_to_handset',0,8),(5,'sim2','x','m','delivered_to_handset',NULL,9);");
    exec_sql(db, "INSERT INTO call_log VALUES('sim1',0),('sim1',1),('sim2',1);");
    assert(sms_paging_count(db, "sim1", &a.sms_total) == 0);
    assert(sim_metrics_read(db, "sim1", &a) == 0);
    assert(a.sms_total == 7 && a.sms_undelivered == 5 && a.calls_total == 2);
    assert(sms_paging_count(db, "sim2", &b.sms_total) == 0);
    assert(sim_metrics_read(db, "sim2", &b) == 0);
    assert(b.sms_total == 2 && b.sms_undelivered == 1 && b.calls_total == 1);
    sqlite3_close(db);
}

/** Сценарий: проверить переходные и конечные статусы обеих production-очередей на настоящей
 * SQLite, в том числе queued, failed, delivering, все заявленные MO states и nullable SRR.
 * Предусловия: для mt_queue state хранит статус доставки SIP, для mt_out state — состояние
 * доставки на handset; SRR может быть NULL до получения отчёта, но это само по себе не доставка.
 * Проверки: queued/new/retrying/failed/delivering и submitted_to_modem/smsc_ok/failed остаются
 * недоставленными; только mt_queue.delivered и mt_out.delivered_to_handset исключаются.
 * В частности, NULL SRR при submitted_to_modem не обнуляет счётчик, а delivered_to_handset
 * при NULL SRR всё равно исключается по state. Проверяется также SMS total обоих потоков.
 * Предотвращаемая регрессия: перепутать queued с delivered, классифицировать по SRR вместо state,
 * считать failed доставленным или учитывать доставленный статус как pending.
 */
static void test_real_status_transitions(void)
{
    sqlite3 *db = NULL;
    struct sim_metrics metrics;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    exec_sql(db, "CREATE TABLE mt_queue(device TEXT, mid TEXT, from_num TEXT, msg TEXT, ts TEXT, state TEXT, created_at INTEGER, attempts INTEGER, next_retry_at INTEGER, last_result TEXT, sent_at INTEGER, delivered_at INTEGER);");
    exec_sql(db, "CREATE TABLE mt_out(id INTEGER, device TEXT, dst TEXT, msg TEXT, ts TEXT, state TEXT, srr_status INTEGER, mr INTEGER, created_at INTEGER, updated_at INTEGER);");
    exec_sql(db, "CREATE TABLE call_log(device TEXT);");
    exec_sql(db, "INSERT INTO mt_queue(device,mid,from_num,msg,state,created_at) VALUES"
                 "('sim1','a','x','m','new',1),('sim1','b','x','m','retrying',2),"
                 "('sim1','c','x','m','queued',3),('sim1','d','x','m','delivering',4),"
                 "('sim1','e','x','m','failed',5),('sim1','f','x','m','delivered',6);");
    exec_sql(db, "INSERT INTO mt_out(id,device,dst,msg,state,srr_status,created_at) VALUES"
                 "(1,'sim1','x','m','submitted_to_modem',NULL,1),(2,'sim1','x','m','smsc_ok',NULL,2),"
                 "(3,'sim1','x','m','failed',64,3),(4,'sim1','x','m','delivered_to_handset',NULL,4);");
    assert(sim_metrics_read(db, "sim1", &metrics) == 0);
    assert(sms_paging_count(db, "sim1", &metrics.sms_total) == 0);
    assert(metrics.sms_total == 10);
    assert(metrics.sms_undelivered == 8);
    sqlite3_close(db);
}

/** Сценарий: открыть настоящую пустую in-memory SQLite с таблицами SMS, но без строк, затем
 * удалить обязательную таблицу звонков и повторить чтение; дополнительно проверить NULL output.
 * Предусловия: SMS-схема достаточна для успешного подсчёта, а call_log обязателен для metrics.
 * Проверки: успешная пустая SIM даёт ровно три нуля, удалённая таблица и NULL output дают -1.
 * Предотвращаемая регрессия: SQL/schema failure маскируется успешными нулями либо wrapper/helper
 * разыменовывает отсутствующий результат вместо сообщения об ошибке.
 */
static void test_zero_and_database_errors(void)
{
    sqlite3 *db = NULL;
    struct sim_metrics metrics;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    exec_sql(db, "CREATE TABLE mt_queue(device TEXT,mid TEXT,from_num TEXT,msg TEXT,ts TEXT,state TEXT,created_at INTEGER,attempts INTEGER,next_retry_at INTEGER,last_result TEXT,sent_at INTEGER,delivered_at INTEGER);");
    exec_sql(db, "CREATE TABLE mt_out(id INTEGER,device TEXT,dst TEXT,msg TEXT,ts TEXT,state TEXT,srr_status INTEGER,mr INTEGER,created_at INTEGER,updated_at INTEGER);");
    exec_sql(db, "CREATE TABLE call_log(device TEXT);");
    assert(sms_paging_count(db, "sim0", &metrics.sms_total) == 0);
    assert(sim_metrics_read(db, "sim0", &metrics) == 0);
    assert(metrics.sms_total == 0 && metrics.sms_undelivered == 0 && metrics.calls_total == 0);
    exec_sql(db, "DROP TABLE call_log;");
    assert(sim_metrics_read(db, "sim0", &metrics) == -1);
    assert(sim_metrics_read(db, "sim0", NULL) == -1);
    sqlite3_close(db);
}

/** Сценарий: отрендерить реальные значения счётчиков в production HTML helper с SIM-именем,
 * содержащим HTML-метасимволы, затем проверить выход слишком малого буфера.
 * Предусловия: структура содержит значения, полученные счетчиком (7/5/2), а renderer использует
 * ограниченный caller-owned buffer.
 * Проверки: видны все три метки и десятичные числа, имя экранировано, исходные теги отсутствуют,
 * нехватка буфера приводит к -1, а не к частичному успешному результату.
 * Предотвращаемая регрессия: XSS через имя SIM, пропуск/перестановка счётчиков и усечение HTML.
 */
static void test_html_values_and_escaping(void)
{
    struct sim_metrics metrics = {7, 5, 2};
    char html[768];
    assert(web_sim_metrics_render(html, sizeof(html), "sim<1&", &metrics) == 0);
    assert(strstr(html, "Всего SMS</th><td>7") != NULL);
    assert(strstr(html, "SMS не доставлено</th><td>5") != NULL);
    assert(strstr(html, "Всего звонков</th><td>2") != NULL);
    assert(strstr(html, "SIM: sim&lt;1&amp;") != NULL);
    assert(strstr(html, "sim<1&") == NULL);
    assert(web_sim_metrics_render(html, 4, "sim1", &metrics) == -1);
}

/** Запустить сценарии счётчиков и HTML; любое нарушение контракта завершает CTest ошибкой. */
int main(void)
{
    test_mapping_and_isolation();
    test_real_status_transitions();
    test_zero_and_database_errors();
    test_html_values_and_escaping();
    puts("per-SIM metrics checks passed");
    return 0;
}
