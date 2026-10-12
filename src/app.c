/*
    app.c
*/

#include "ast_config.h"

#include <asterisk/app.h> /* AST_DECLARE_APP_ARGS() ... */
#include <asterisk/json.h>
#include <asterisk/module.h> /* ast_register_application2() ast_unregister_application() */
#include <asterisk/pbx.h>    /* pbx_builtin_setvar_helper() */
#include <asterisk/strings.h>

#include "app.h" /* app_register() app_unregister() */

#include "chan_quectel.h" /* struct pvt */
#include "error.h"
#include "helpers.h" /* send_sms() ITEMS_OF() */
#include "smsdb.h"

struct ast_channel;

/*** DOCUMENTATION
    <function name="QUECTEL_STATUS" language="en_US">
        <synopsis>
            Gets status of a channel in a numerical value.
        </synopsis>
        <syntax>
            <parameter name="resource" required="true">
                <para>Resource string as for Dial().</para>
            </parameter>
        </syntax>
        <description>
            <para>Returns integer value:<para>
            <para>1 - not started</para>
            <para>2 - available</para>
            <para>3 - unabailable</para>
        </description>
    </function>
    <function name="QUECTEL_STATUS_EX" language="en_US">
        <synopsis>
            Gets status of a channel in a JSON value.
        </synopsis>
        <syntax>
            <parameter name="resource" required="true">
                <para>Resource string as for Dial().</para>
            </parameter>
        </syntax>
    </function>
    <application name="QUECTEL_SEND_SMS" language="en_US">
        <synopsis>
            Sends a SMS on specified device.
        </synopsis>
        <syntax>
            <parameter name="resource" required="true">
                <para>Resource string as for Dial().</para>
            </parameter>
            <parameter name="destination" required="true">
                <para>Recipient.</para>
            </parameter>
            <parameter name="message" required="true">
                <para>Text of the message.</para>
            </parameter>
            <parameter name="validity" required="false">
                <para>Validity period in minutes.</para>
            </parameter>
            <parameter name="report" required="false">
                <para>Boolean flag for report request.</para>
            </parameter>
            <parameter name="imdn_id" required="false">
                <para>IMDN message id (SIP Call-ID of the incoming MO MESSAGE) recorded with the message.</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_SEND_USSD</ref>
        </see-also>
    </application>
    <application name="QUECTEL_MT_RECEIPT" language="en_US">
        <synopsis>
            Handle an incoming IMDN delivery receipt (message/imdn+xml) on a Message channel
            and mark the correlated MT queue row as delivered.
        </synopsis>
        <syntax>
            <parameter name="resource" required="false">
                <para>Unused; the handler reads MESSAGE_DATA(Content-Type)/MESSAGE(body).</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_SEND_SMS</ref>
        </see-also>
    </application>
    <application name="QUECTEL_SEND_USSD" language="en_US">
        <synopsis>
            Sends a USSD on specified device.
        </synopsis>
        <syntax>
            <parameter name="device" required="true">
                <para>Id of device from configuration file.</para>
            </parameter>
            <parameter name="destination" required="true">
                <para>Recipient.</para>
            </parameter>
            <parameter name="ussd" required="true">
                <para>USSD command.</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_SEND_SMS</ref>
        </see-also>
    </application>
    <application name="QUECTEL_DELETE_SMS" language="en_US">
        <synopsis>
            Deletes a SMS on specified device.
        </synopsis>
        <syntax>
            <parameter name="device" required="true">
                <para>Id of device from configuration file.</para>
            </parameter>
            <parameter name="idx" required="true">
                <para>Index of the message.</para>
            </parameter>
            <parameter name="delflag" required="false">
                <para>Deletion flag.</para>
            </parameter>
        </syntax>
        <see-also>
            <ref type="application">QUECTEL_DELETE_SMS</ref>
        </see-also>
    </application>
 ***/

static int app_status_read(struct ast_channel* channel, const char* cmd, char* data, struct ast_str** str, ssize_t maxlen)
{
    int stat;
    int exists = 0;

    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(resource);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.resource)) {
        return -1;
    }

    RAII_VAR(struct pvt* const, pvt, pvt_find_by_resource(args.resource, 0, NULL, &exists), pvt_unlock);
    if (pvt) {
        stat = 2;
    } else {
        stat = exists ? 3 : 1;
    }

    ast_str_set(str, maxlen, "%d", stat);
    return 0;
}

static int app_status_ex_read(struct ast_channel* channel, const char* cmd, char* data, struct ast_str** str, ssize_t maxlen)
{
    int exists = 0;

    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(resource);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.resource)) {
        return -1;
    }

    RAII_VAR(struct ast_json* const, status, ast_json_object_create(), ast_json_unref);
    RAII_VAR(struct pvt* const, pvt, pvt_find_by_resource(args.resource, CALL_FLAG_INTERNAL_REQUEST, NULL, &exists), pvt_unlock);
    if (pvt) {
        ast_json_object_set(status, "resource", ast_json_string_create(args.resource));
        pvt_get_status(pvt, status);
    } else {
        ast_json_object_set(status, "resource", ast_json_string_create(args.resource));
        ast_json_object_set(status, "exists", ast_json_integer_create(0));
    }

    ast_json_dump_str_format(status, str, AST_JSON_COMPACT);
    return 0;
}

static int parse_validity(const char* const arg)
{
    static const int DEF_VALIDITY = 15;

    if (ast_strlen_zero(arg)) {
        return DEF_VALIDITY;
    }

    int validity;
    const int res = sscanf(arg, "%d", &validity);

    return res && (validity > 0) ? validity : DEF_VALIDITY;
}

static int parse_report_flag(const char* arg)
{
    static const int DEF_REPORT_FLAG = 0;

    if (ast_strlen_zero(arg)) {
        return DEF_REPORT_FLAG;
    }

    if (ast_true(arg)) {
        return 1;
    }

    if (ast_false(arg)) {
        return 0;
    }

    return DEF_REPORT_FLAG;
}

static unsigned int parse_idx(const char* const arg)
{
    static const unsigned int DEF_IDX = 0;

    if (ast_strlen_zero(arg)) {
        return DEF_IDX;
    }

    unsigned int idx;
    const int res = sscanf(arg, "%u", &idx);

    return res ? idx : DEF_IDX;
}

static int parse_delflag(const char* const arg)
{
    static const int DEF_DELFLAG = 0;

    if (ast_strlen_zero(arg)) {
        return DEF_DELFLAG;
    }

    int delflag;
    const int res = sscanf(arg, "%d", &delflag);

    return res ? delflag : DEF_DELFLAG;
}

static int app_send_sms_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(resource);
        AST_APP_ARG(number);
        AST_APP_ARG(message);
        AST_APP_ARG(validity);
        AST_APP_ARG(report);
        AST_APP_ARG(imdn_id);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.resource)) {
        ast_log(LOG_ERROR, "NULL resource for message -- SMS will not be sent\n");
        return -1;
    }

    if (ast_strlen_zero(args.number)) {
        ast_log(LOG_ERROR, "NULL destination for message -- SMS will not be sent\n");
        return -1;
    }

    /* Record the outgoing SMS for MT delivery status tracking (honest state set by send_sms result below). */
    const int mt_out_id = smsdb_mt_out_add(args.resource, args.number, args.message, "", S_OR(args.imdn_id, ""));
    ast_log(LOG_NOTICE, "[%s] MT: outgoing SMS recorded in mt_out (id=%d dst=%s%s%s)\n",
        args.resource, mt_out_id, args.number, ast_strlen_zero(args.imdn_id) ? "" : " imdn_id=",
        ast_strlen_zero(args.imdn_id) ? "" : args.imdn_id);

    if (send_sms(args.resource, "", args.number, args.message, parse_validity(args.validity), parse_report_flag(args.report), mt_out_id)) {
        ast_log(LOG_ERROR, "[%s] MT: outgoing SMS refused by modem, mt_out #%d -> failed (%s)\n",
            args.resource, mt_out_id, error2str(chan_quectel_err));
        /* The modem refused the message: do not leave it marked as submitted. */
        if (mt_out_id > 0) {
            smsdb_mt_out_set_state(mt_out_id, "failed", 0);
        }
        return -1;
    }

    ast_log(LOG_NOTICE, "[%s] MT: outgoing SMS handed to modem (mt_out id=%d dst=%s)\n",
        args.resource, mt_out_id, args.number);
    return 0;
}

static int app_mt_receipt_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    char ctype[128] = "";
    char body[8192] = "";
    char mid[MT_MID_LEN];
    const char* st;
    const char* en;

    if (!channel) {
        return 0;
    }

    ast_func_read(channel, "MESSAGE_DATA(Content-Type)", ctype, sizeof(ctype));
    ast_func_read(channel, "MESSAGE(body)", body, sizeof(body));
    body[sizeof(body) - 1] = '\0';

    /* Only handle IMDN delivery notifications; leave every other MESSAGE alone. */
    {
        char ctype_clean[sizeof(ctype)];
        char* params;

        ast_copy_string(ctype_clean, ctype, sizeof(ctype_clean));
        params = strchr(ctype_clean, ';');
        if (params) {
            *params = '\0';
        }
        ast_trim_blanks(ctype_clean);
        if (strcasecmp(ctype_clean, "message/imdn+xml") != 0) {
            ast_log(LOG_NOTICE, "mt: QUECTEL_MT_RECEIPT: ignoring non-IMDN MESSAGE content-type '%s'\n", ctype);
            return 0;
        }
    }

    if (ast_strlen_zero(body)) {
        ast_log(LOG_WARNING, "mt: QUECTEL_MT_RECEIPT: empty IMDN body\n");
        return 0;
    }

    /* No XML library: simple substring search for <message-id>...</message-id>. */
    st = strstr(body, "<message-id>");
    en = strstr(body, "</message-id>");
    if (!st || !en || en <= st) {
        ast_log(LOG_WARNING, "mt: QUECTEL_MT_RECEIPT: no <message-id> in IMDN body\n");
        return 0;
    }
    st += strlen("<message-id>");
    {
        size_t n = (size_t)(en - st);
        if (n >= sizeof(mid)) {
            n = sizeof(mid) - 1;
        }
        memcpy(mid, st, n);
        mid[n] = '\0';
    }
    if (ast_strlen_zero(mid)) {
        ast_log(LOG_NOTICE, "mt: QUECTEL_MT_RECEIPT: empty message-id\n");
        return 0;
    }
    if (!strstr(body, "<delivered")) {
        ast_log(LOG_NOTICE, "mt: QUECTEL_MT_RECEIPT: message-id %s has no <delivered> status, ignoring\n", mid);
        return 0;
    }

    /* Correlate with the MT queue row and mark it as delivered via IMDN. */
    struct smsdb_mt_row row;
    if (smsdb_mt_get(mid, &row)) {
        ast_log(LOG_NOTICE, "mt: QUECTEL_MT_RECEIPT: unknown mid %s, ignoring IMDN receipt\n", mid);
        return 0;
    }

    const long now = time(NULL);
    smsdb_mt_set_result(mid, "delivered", row.attempts, 0, "delivered via IMDN", (int)now, (int)now);
    ast_log(LOG_NOTICE, "mt: IMDN receipt mid=%s -> delivered via IMDN\n", mid);
    return 0;
}

static int app_send_ussd_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(device);
        AST_APP_ARG(ussd);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.device)) {
        ast_log(LOG_ERROR, "NULL device for ussd -- USSD will not be sent\n");
        return -1;
    }

    if (ast_strlen_zero(args.ussd)) {
        ast_log(LOG_ERROR, "NULL ussd command -- USSD will not be sent\n");
        return -1;
    }

    if (send_ussd(args.device, args.ussd)) {
        ast_log(LOG_ERROR, "[%s] %s\n", args.device, error2str(chan_quectel_err));
        return -1;
    }

    return 0;
}

static int app_delete_sms_exec(attribute_unused struct ast_channel* channel, const char* data)
{
    /* clang-format off */

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(device);
        AST_APP_ARG(idx);
        AST_APP_ARG(delflag);
    );

    /* clang-format on */

    if (ast_strlen_zero(data)) {
        return -1;
    }

    char* const parse = ast_strdupa(data);

    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.device)) {
        ast_log(LOG_ERROR, "NULL device for message -- SMS will not be deleted\n");
        return -1;
    }

    if (ast_strlen_zero(args.idx)) {
        ast_log(LOG_ERROR, "NULL idx for message -- SMS will not be deleted\n");
        return -1;
    }

    if (delete_sms(args.device, parse_idx(args.idx), parse_delflag(args.delflag))) {
        ast_log(LOG_ERROR, "[%s] %s\n", args.device, error2str(chan_quectel_err));
        return -1;
    }

    return 0;
}

/* clang-format off */

static struct ast_custom_function status_function = {
    .name = "QUECTEL_STATUS",
    .read2 = app_status_read,
    .read_max = 2
};

static struct ast_custom_function status_ex_function = {
    .name = "QUECTEL_STATUS_EX",
    .read2 = app_status_ex_read,
};

/* clang-format on */

static const char APP_SEND_SMS[]   = "QUECTEL_SEND_SMS";
static const char APP_SEND_USSD[]  = "QUECTEL_SEND_USSD";
static const char APP_DELETE_SMS[] = "QUECTEL_DELETE_SMS";
static const char APP_MT_RECEIPT[] = "QUECTEL_MT_RECEIPT";

int app_register()
{
    int res;

    status_function.mod    = self_module();
    status_ex_function.mod = self_module();

    res  = ast_custom_function_register(&status_function);
    res |= ast_custom_function_register(&status_ex_function);
    res |= ast_register_application2(APP_SEND_SMS, app_send_sms_exec, NULL, NULL, self_module());
    res |= ast_register_application2(APP_SEND_USSD, app_send_ussd_exec, NULL, NULL, self_module());
    res |= ast_register_application2(APP_DELETE_SMS, app_delete_sms_exec, NULL, NULL, self_module());
    res |= ast_register_application2(APP_MT_RECEIPT, app_mt_receipt_exec, NULL, NULL, self_module());

    return res;
}

void app_unregister()
{
    ast_unregister_application(APP_DELETE_SMS);
    ast_unregister_application(APP_SEND_USSD);
    ast_unregister_application(APP_SEND_SMS);
    ast_unregister_application(APP_MT_RECEIPT);
    ast_custom_function_unregister(&status_ex_function);
    ast_custom_function_unregister(&status_function);
}
