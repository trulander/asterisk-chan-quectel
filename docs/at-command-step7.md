# Шаг 7 плана 10: отладочная AT-страница mini-web

## Назначение и файлы

Страница позволяет оператору выбрать настроенное SIM-устройство, указать AT-команду и явно отправить форму. Она показывает только немедленный результат enqueue/error. Нового listener, порта или bind address нет; SMS delivery/retry и другие маршруты не меняются.

- `src/mt_task.c` — интеграция GET/POST `/at` в уже работающий mini-web handler, адаптер `recv()` и привязка к текущему `send_at_command()`.
- `src/web_at_command.[ch]` — production-used ограниченное чтение HTTP, разбор формы, submit seam и builder HTTP response. Вынесение позволяет тестировать настоящие parsing/formatting функции без Asterisk и без AT-вызовов.
- `src/CMakeLists.txt` — включение seam в модуль драйвера.
- `test/web_at_command_test.c` и `test/CMakeLists.txt` — regression проверки parser, partial reads, framing, fake queue adapter и точных HTTP response.

## Маршруты и форма

| Метод | URL | Поведение |
|---|---|---|
| GET | `/at` | Строит и показывает форму; enqueue не вызывается. |
| POST | `/at` | Проверяет framing, MIME, поля и выбранное устройство; затем ровно один раз вызывает `send_at_command()`. |

POST использует `application/x-www-form-urlencoded`; обязательны ровно `device` и `command`. `device` должен совпадать с настроенным устройством. Декодированное имя ограничено 63 байтами; команда — 1–512 байт; тело — максимум 640 байт. Пустые/повторные/лишние поля, пустая пара после завершающего `&`, некорректный `%HH`, NUL, CR/LF и остальные управляющие символы отвергаются. Другие AT-семантические ограничения не вводятся.

HTTP reader накапливает частичные чтения до `CRLF CRLF` и затем читает ровно Content-Length. Он обрабатывает один request frame: если callback уже вернул байты сверх конца заголовка и заявленного тела, запрос отвергается; когда frame завершён точно, reader останавливается и не делает peek/дополнительное чтение. Позднее поступившие байты следующего pipelined запроса reader не отвергает и не обрабатывает. `mt_web_serve_client()` обслуживает один запрос на соединение и закрывает клиентский socket после ответа, поэтому следующий запрос не получает отдельного HTTP-ответа. Заголовок максимум 2048 байт. Content-Length должен быть единственным непустым полем из десятичных цифр без знака и хвостового мусора; overflow/значение выше лимита отвергаются. Дубликаты Content-Type, неподдерживаемый MIME и любой Transfer-Encoding отвергаются; тело без однозначного Content-Length не принимается. Заголовки ищутся только внутри ограниченного header area. У callback чтения есть таймаут на каждое ожидание данных.

## HTTP ответы

- GET `/at`: `200 OK`, `text/html; charset=utf-8`, форма и предупреждение, что результат команды появится в логах Asterisk.
- Валидный POST, enqueue успешен: `200 OK`, HTML «Команда поставлена в очередь».
- Ошибка enqueue: `503 Service Unavailable`, HTML «Ошибка постановки команды в очередь».
- Ошибка формы/framing/MIME/SIM: `400 Bad Request` (для неизвестного device отдельное plain-text сообщение); HTTP reader fail-closed также отвечает 400.
- Ошибка выделения/построения страницы: `500 Internal Server Error`.

`web_at_response_build()` формирует production body/status/MIME и HTML-экранирует устройство/команду. Поля с AT-командой не вставляются в HTML напрямую. Форма и result response содержат русское пояснение: enqueue не означает выполнение, фактический ответ искать в логах Asterisk.

## Путь команды и честная семантика

POST проходит `web_at_http_read()` → HTTP/parser проверки → `web_at_submit()` → проверка `mt_web_sim_exists()` → adapter `mt_web_at_enqueue()` → `send_at_command(device, command)` → `at_enqueue_user_cmd()` → очередь CMD_USER. `send_at_command()` возвращает -1, если устройство не найдено, иначе возвращает результат `at_enqueue_user_cmd()`. Последний возвращает 0 только если задача добавлена; при ошибке очереди устанавливает `E_QUEUE` и возвращает -1. Никакого ожидания ответа модема API не делает.

Фактический AT response обрабатывается позже общей очередью AT и логируется вне HTTP запроса. Нет correlation ID для произвольной команды. Поэтому queued/error — единственные результаты, которые страница сообщает; она не выдаёт enqueue за синхронное выполнение.

## Тесты и границы

`web_at_command_test` вызывает production parser/read/response/submit seam. Проверяются 1 и 512-байтные команды, отклонение 513 байт, пустые/дублированные/лишние поля и trailing `&`, percent/control/NUL, лимит device/body, один enqueue выбранного sim1, отсутствие enqueue при malformed/unknown device, ошибка adapter, fragmented header/body, incomplete body, missing/garbage/signed/duplicate Content-Length, duplicate/unsupported Content-Type, Transfer-Encoding, coalesced extra bytes rejection и отдельный сценарий, где следующий запрос появляется только после завершения frame (reader его не читает), а также response status/MIME/body и HTML escaping.

Fake enqueue не обращается к оборудованию. Отдельный тест фактического Asterisk socket/static handler не выполнялся: он зависит от `pvt_status_each`, Asterisk state и сокета. Проверяемые production seams используются этим handler непосредственно; единственный непокрытый переход — конкретный вызов `send_at_command()` адаптером с Asterisk `pvt` контекстом. Его контракт проверен по `src/helpers.c` и `src/at_command.c`; runtime/device проверка не выполнялась. Тесты не отправляют AT-команды.

## Почему не показываются ответы и не разбираются логи

Ответ модема асинхронен и не связан с HTTP-запросом устойчивой идентичностью. Общий лог содержит ответы разных AT-команд и устройств. Tail/parser общего журнала мог бы присвоить странице чужой ответ и ложно показать его как результат submit; такой механизм намеренно не добавлен.
