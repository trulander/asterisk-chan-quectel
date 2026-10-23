# Шаг 6: web-история звонков

## Контракт

Добавлены только per-SIM `GET /calls/<sim>` (HTML) и `GET /calls/<sim>.json?page=N` (JSON); общего `/calls` index нет. SIM валидируется существующим parser/existence-check. Параметр `page` использует существующий parser; страницы нумеруются с 1, значения вне границ clamp-ятся к `[1, pages]`. При нулевом total отображается пустая страница 1/1. Лимит SQL — строго 30 строк; последняя страница содержит остаток.

JSON envelope: `{ "device": "sim1", "page": 1, "page_size": 30, "total": 1, "pages": 1, "items": [...] }`. Item: `id` (`call-<rowid>`), `device`, `number`, `started_at` (сохранённое UTC), `direction` (incoming/outgoing/missed), `answered`, `finished`, `ended_at` (строка либо JSON null). HTML колонки: durable ID, номер, начало UTC, направление/тип, answered, finished, окончание UTC. `call_idx` не читается рендерером и не публикуется. SQL использует точный `WHERE device=?` и `ORDER BY started_at DESC,id DESC`, tie-break по durable ID.

## Escaping и HTTP errors

Все значения из БД, включая durable ID/номер/времена/direction, в HTML передаются через существующий `mt_html_escape`; query links формируются только из имени SIM, предварительно ограниченного `/^[A-Za-z0-9_.-]+$/`, поэтому path сегмент не может содержать HTML/URL delimiters. JSON string fields строятся `ast_json_string_create` и сериализуются Asterisk JSON API (JSON escaping). Любой DB/count/page, JSON object/value construction, setter/append либо serialization error отбрасывает partial envelope и отвечает HTTP 500. Общий JSON error body — `CALL_WEB_JSON_ERROR`; длина в HTTP задаётся `sizeof(CALL_WEB_JSON_ERROR)-1` (37 байт). JSON count failure получает `application/json`, HTML ошибки сохраняют `text/plain`.

## Lock / ownership

SMSDB wrappers держат общий DB mutex весь вызов pager; callback получает временную копию row, валидную только на время вызова. Возврат callback `0` продолжает обход; любой nonzero прекращает обход, но `call_paging_page()` возвращает `0` как успешный early-stop, а не callback failure. Поэтому callback-specific ошибку caller обязан отдельно сохранить в контексте (sticky flag) и проверить после возврата pager; JSON renderer использует для этого `ctx->failed`. Callback не пишет в socket. Для Asterisk JSON API сверены bundled `include/asterisk/json.h` и `main/json.c`: `ast_json_string_create` возвращает NULL при allocation failure; `ast_json_object_set`/`ast_json_array_append` возвращают 0 успех/-1 ошибку, и принимающий object/array забирает переданный value reference даже при ошибке. Поэтому builder заранее отвергает NULL value, проверяет все возвраты и использует дополнительный `ast_json_ref` там, где caller должен сохранить локальное владение для cleanup. Локальные значения/массив/root освобождаются ровно одним `ast_json_unref`; serialization string — `ast_json_free`.

## Изменения и проверки

Изменены `src/call_paging.[ch]` (production SQL seam), `src/smsdb.[ch]` (mutex wrappers), `src/mt_task.c` (routes/render), `src/CMakeLists.txt`, `test/CMakeLists.txt`, `test/call_paging_test.c` и данный документ. `call-paging` CTest использует настоящую in-memory SQLite и production pager: exact SIM/foreign SIM, SQL-like device, все выбранные поля/NULL/truncation, descending ordering/tie, страницы 30/30/остаток, empty/out-of-range, page metadata clamping, malformed schema, step-time SQL error, early stop и production call lifecycle rows. Тест отдельно проверяет, что `sizeof(CALL_WEB_JSON_ERROR)-1 == 37`.

В standalone CTest нет Asterisk JSON runtime/allocator hook: `ast_json_set_alloc_funcs` изменяет allocator глобально для всех JSON-пользователей, поэтому опасно fault-inject-ить его в тестовом процессе драйвера. Также HTTP static renderers привязаны к Asterisk `ast_str`/socket API и не вызываются в pager CTest; HTML/URL/JSON escaping и allocation/setter failure требуют следующего tester этапа с подходящим Asterisk harness либо runtime fault injection. Тесты не являются сквозной проверкой HTTP/Asterisk, persistent DB file или устройства. Устройство не использовалось.
