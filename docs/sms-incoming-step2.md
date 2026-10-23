# Входящие SMS: сохранение частей и безопасное удаление из SIM (step 2)

## Область и файлы

Функция охватывает только incoming SMS из SIM-хранилища `SM` и текущий step 2 плана. `ME` не обрабатывается; `RES_CMT` приходит без ячейки и никогда не удаляется. `sms_autodelete` остаётся обязательным gate.

- `src/at_parse.c`, `src/at_parse.h`: сохраняют storage из `+CMTI`, чтобы не терять привязку к ячейке.
- `src/at_command.c`, `src/at_command.h`: перед чтением/листингом выбирают `SM`; создают `CMGD` и свежий `CMGL` scan.
- `src/at_queue.c`, `src/at_queue.h`: переносят rowid и verify-rowid вместе с AT task, чтобы ответы связывались с durable записью, а не только с переиспользуемым индексом.
- `src/at_response.c`: соединяет modem read, сохранение, queueing и удаление; верификация после неопределённого `CMGD` всегда читает актуальную SIM-ячейку.
- `src/mt_task.c`, поля в `src/chan_quectel.h`/инициализация `src/chan_quectel.c`: периодический retry для durable delete jobs и временных read/DB ошибок.
- `src/smsdb.c`, `src/smsdb.h`: транзакционный multipart insert/assembly, durable delete-state, queue marker и GC.
- `src/sms_multipart.[ch]`: production-used SQLite insert/assembly и вставка в `mt_queue`; assembly использует `count == parts` и `ORDER BY seqorder`.
- `src/sms_incoming_schema.[ch]`: production-used миграция и небольшие SQLite операции, которые также доступны тестам без Asterisk.
- `src/sms_delete_state.[ch]`: dependency-light таблица переходов delete status/attempts.
- `src/sms_delete_flow.[ch]`: production-used порядок действий для уже прочитанной ячейки: durable save → при полной SMS durable MT queue → CMGD; отдельно обрабатывает absent/read-error и gate.
- `test/sms_multipart_test.c`, `test/sms_delete_state_test.c`, `test/sms_delete_flow_test.c`, `test/sms_delete_flow_sqlite_test.c`: migration, rowid isolation, multipart, retry transitions, SQLite-backed queue/GC/restart and scheduler fairness regression tests.

## `incoming_msg`

Новая/мигрированная таблица сохраняет multipart строки до завершения безопасной очистки. Новые поля:

| Поле | Значение |
| --- | --- |
| `source_device`, `source_storage`, `source_index` | Устройство и SIM-ячейка, из которой прочитана часть (`SM`, индекс). Для `RES_CMT` значения не задаются. |
| `delete_status` | `0 pending`, `1 verify`, `2 complete`, `3 stopped`, `4 CMGD sent`, `5 verifying counted`, `6 verifying uncounted`. |
| `delete_attempts` | Число подтверждённых неудач: CMGD завершился ошибкой/timeout, затем fresh read нашёл SMS, и сегмент был durable-сохранён. |
| `retry_at` | Время следующего периодического действия. Ошибки чтения/БД/COMMIT не увеличивают счётчик. |
| `mt_queued` | Полная SMS уже durable находится в `mt_queue`; это условие GC, а не Flexisip delivery status. |
| `mt_mid` | MID уже созданной MT queue row для стабильной incoming-группы; предотвращает создание новой строки при повторном чтении той же группы. |

Миграция idempotent: старые записи получают defaults, источники остаются NULL и не становятся SIM delete jobs. Обновление delete state требует одновременно выбранный `rowid` и совпадающие device/storage/index; слот, повторно занятый другой SMS, не изменяется.

## Поток и восстановление

1. Для `CMTI` принимается только `SM`; перед `CMGR` устанавливается `CPMS="SM"`. `CMGL` verification тоже выбирает `SM`.
2. Прочитанная SIM-часть сохраняется и COMMIT выполняется под per-connection SQLite mutex. Multipart собирается только когда число строк равно `parts`, в `seqorder`; strict sequence-set validation не добавляется.
3. Неполная SMS не попадает в MT delivery queue. Её durable segment можно удалить из SIM, потому что сама часть уже сохранена; строки БД удерживаются до полной MT очереди и отсутствия всех соответствующих ячеек.
4. Для полной SMS сначала проверяется durable group marker; новая MID генерируется только если группа ещё не queued. MT row и marker (`mt_queued` + `mt_mid`) вставляются в одной mutex-held SQLite transaction; только после commit ставится `CMGD`. Повторное чтение той же durable группы возвращает прежний MID и не вставляет лишнюю MT row. Flexisip delivery не является условием очистки.
5. Ответ `CMGD OK` подтверждает удаление. После ERROR/timeout старый `CMGD` не повторяется: ставится свежая выборка `CMGL`, только отсутствие подтверждает успех; если содержимое присутствует, оно сохраняется заново и лишь потом планируется новый `CMGD`.
6. `VERIFY_PRESENT_SAVED` увеличивает attempts именно после предыдущего CMGD ERROR/TIMEOUT + fresh read present + durable save. Успешный следующий `CMGD` счётчик не расходует. После пяти подтверждённых неудач статус `stopped` исключён из scheduler.
7. При restart статусы `CMGD sent` и interrupted verify выбираются периодическим scheduler и восстанавливаются свежим чтением. `CMGD sent` переводится в uncounted verification: если команда могла не дойти до модема, одно лишь наличие ячейки не засчитывается как подтверждённый отказ. Если сохранённый counted verification связан с предыдущей CMGD ошибкой, счётчик сохраняется.
8. Временно недоступное, неинициализированное, занятое verification или `sms_autodelete=off` устройство не блокирует остальные due jobs: scheduler сдвигает только `retry_at`, сохраняя status/attempts; при отключённом gate AT команда не ставится.
9. Atomic group marker устраняет avoidable duplicates при обычном повторном чтении той же retained group. Exactly-once не обещается: если crash случится после полного GC сегментов, а позже та же SIM SMS вновь появится/будет прочитана как новая группа, у неё не останется marker mapping; принятое решение допускает повторную enqueue/delivery в таком ambiguous re-read сценарии.
10. DB/COMMIT/read ошибки не приводят к CMGD. Для ранее durable строк повтор запускает scheduler; временная ошибка до первой durable записи сохраняется в per-device retry fields, пока работает процесс.

## GC и SQLite locking

`incoming_msg` удаляется только когда одновременно `MIN(mt_queued)=1` и все строки группы имеют `delete_status=complete`. Это проверяется после queue marker и после подтверждения очередной пустой ячейки; одна пустая ячейка не удаляет остальные сегменты.

Каждый новый SQL helper, изменяющий строки в production, вызывается внутри общей транзакции владельца:

- `smsdb_put_internal`: `sms_tx_lock` до BEGIN; multipart insert/read/assembly и COMMIT/ROLLBACK до unlock.
- `smsdb_mt_queue_incoming_group`: mutex удерживается от BEGIN через проверку stable marker, MT insert, MID marker и eligible GC до COMMIT/ROLLBACK; откат не оставляет orphan queue row или ложный marker.
- `smsdb_incoming_delete_apply`: mutex удерживается от state read через rowid+identity update/GC до COMMIT/ROLLBACK.
- `smsdb_incoming_delete_defer`: для недоступного/disabled/busy устройства mutex удерживается на retry_at-only update; delete status и attempts не меняются.
- `db_create`: `SMS_TX_SCOPE` удерживает mutex для миграций; ошибка миграции/DDL выполняет ROLLBACK и помечает scope failed, чтобы cleanup не пытался COMMIT.

SQLite helper-функции в `sms_incoming_schema.c` сами mutex не захватывают: production callers обязаны удерживать его и включать mutations в транзакцию. Тесты вызывают их на изолированной in-memory/file DB без конкурирующих потоков.

## Тестирование

Собрать dependency-light тесты через CTest: `sms-multipart`, `sms-delete-state`, `sms-delete-flow`, `sms-delete-flow-sqlite`. Полный driver build требует Asterisk include root; для локального cached source build передавайте его через `CMAKE_INCLUDE_PATH` и include root через `CMAKE_C_FLAGS`, если проектный target не добавил `${MT_AST_INCLUDE_DIR}` в include list.
