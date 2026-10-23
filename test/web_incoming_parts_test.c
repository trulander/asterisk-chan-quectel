#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "web_incoming_parts.h"
static int checks, failures;
/* CHECK учитывает каждую ветвь контракта и продолжает выполнение при assertion failure. */
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); } } while (0)
/** Проверить production-used route, exact HTTP errors, escaping и URL helper edge cases. */
int main(void)
{
    const char *tail = NULL;
    int json = -1;
    struct web_incoming_error error;
    char escaped[128], url[128], tiny[4];
    const char utf8[] = "caf\xc3\xa9 & < > \" '";
    const char controls[] = "a\tb\nc\rd";
    char long_path[512];
    CHECK(web_incoming_route("/sms/parts", &tail, &json) == WEB_INCOMING_ALL_HTML && !json && !tail);
    CHECK(web_incoming_route("/sms/parts.json", &tail, &json) == WEB_INCOMING_ALL_JSON && json && !tail);
    CHECK(web_incoming_route("/sms/parts/sim-A.json", &tail, &json) == WEB_INCOMING_SIM && json && !strcmp(tail,"sim-A.json"));
    /* Per-SIM JSON является отдельным response mode, а не HTML default. */
    CHECK(web_incoming_route("/sms/parts/sim-A.json", &tail, &json) == WEB_INCOMING_SIM && json);
    CHECK(web_incoming_route("/sms/parts/sim-A", &tail, &json) == WEB_INCOMING_SIM && !json);
    CHECK(web_incoming_route("/sms/parts/.json", &tail, &json) == WEB_INCOMING_SIM && json && !strcmp(tail,".json"));
    CHECK(web_incoming_route("/sms/parts/sim.name.json", &tail, &json) == WEB_INCOMING_SIM && json && !strcmp(tail,"sim.name.json"));
    CHECK(web_incoming_route("/sms/parts/sim.name.json.json", &tail, &json) == WEB_INCOMING_SIM && json && !strcmp(tail,"sim.name.json.json"));
    CHECK(web_incoming_route("/sms/parts/sim-A/extra", &tail, &json) == WEB_INCOMING_SIM && !json && !strcmp(tail,"sim-A/extra"));
    CHECK(web_incoming_route("/sms/parts/sim-A/extra.json", &tail, &json) == WEB_INCOMING_SIM && json && !strcmp(tail,"sim-A/extra.json"));
    CHECK(web_incoming_route("/sms/parts/sim-A.jsonx", &tail, &json) == WEB_INCOMING_SIM && !json && !strcmp(tail,"sim-A.jsonx"));
    CHECK(web_incoming_route("/sms/parts/sim-A.JSON", &tail, &json) == WEB_INCOMING_SIM && !json && !strcmp(tail,"sim-A.JSON"));
    CHECK(web_incoming_route("/sms/parts/", &tail, &json) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route("/sms/partsExtra", &tail, &json) == WEB_INCOMING_NOT_ROUTE);
    /* Новый классификатор не должен перехватывать ни один из этих старых маршрутов. */
    CHECK(web_incoming_route("/json", &tail, &json) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route("/sms", &tail, &json) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route("/sms/sim-A", &tail, &json) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route(NULL, &tail, &json) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route("/sms/parts", NULL, &json) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route("/sms/parts", &tail, NULL) == WEB_INCOMING_NOT_ROUTE);
    CHECK(web_incoming_route("", &tail, &json) == WEB_INCOMING_NOT_ROUTE && tail == NULL && json == 0);
    web_incoming_not_found_response(&error);
    CHECK(!strcmp(error.status,"404 Not Found"));
    CHECK(!strcmp(error.content_type,"text/plain"));
    CHECK(error.body_len == 9 && !strcmp(error.body,"not found"));
    web_incoming_error_response(1, &error);
    CHECK(!strcmp(error.status,"500 Internal Server Error"));
    CHECK(error.body_len == strlen(error.body));
    CHECK(!strcmp(error.content_type,"application/json"));
    CHECK(error.body_len == strlen(error.body));
    CHECK(error.body_len == 36 && !strcmp(error.body,"{\"error\":\"sms database unavailable\"}"));
    CHECK(error.body_len == sizeof("{\"error\":\"sms database unavailable\"}") - 1);
    web_incoming_error_response(0, &error);
    CHECK(!strcmp(error.status,"500 Internal Server Error"));
    CHECK(!strcmp(error.content_type,"text/plain"));
    CHECK(error.body_len == strlen(error.body));
    CHECK(!strcmp(error.body,"internal error: SMS database unavailable"));
    CHECK(error.body_len == sizeof("internal error: SMS database unavailable") - 1);
    CHECK(web_incoming_html_escape("<&>\"'", escaped,sizeof(escaped)) == strlen("&lt;&amp;&gt;&quot;&#39;"));
    CHECK(!strcmp(escaped,"&lt;&amp;&gt;&quot;&#39;"));
    CHECK(web_incoming_html_escape("<unsafe>", escaped, 5) == 14 && escaped[4] == '\0');
    CHECK(web_incoming_html_escape("<&>", NULL, 0) == 13);
    CHECK(web_incoming_html_escape(NULL, escaped, sizeof(escaped)) == 0 && !strcmp(escaped, ""));
    CHECK(web_incoming_html_escape(utf8, escaped, sizeof(escaped)) == strlen("caf\xc3\xa9 &amp; &lt; &gt; &quot; &#39;"));
    CHECK(!strcmp(escaped, "caf\xc3\xa9 &amp; &lt; &gt; &quot; &#39;"));
    CHECK(web_incoming_html_escape(controls, escaped, sizeof(escaped)) == strlen(controls));
    CHECK(!memcmp(escaped, controls, sizeof(controls)));
    {
        const char hostile[] = "<script>\"x'&</script>\t\n\xc3\xa9";
        const char expected[] = "&lt;script&gt;&quot;x&#39;&amp;&lt;/script&gt;\t\n\xc3\xa9";
        CHECK(web_incoming_html_escape(hostile, escaped, sizeof(escaped)) == sizeof(expected) - 1);
        CHECK(!strcmp(escaped, expected));
    }
    memset(tiny, 'x', sizeof(tiny));
    CHECK(web_incoming_html_escape("&", tiny, sizeof(tiny)) == 5 && tiny[3] == '\0');
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    CHECK(web_incoming_html_escape(long_path, NULL, 0) == sizeof(long_path) - 1);
    CHECK(web_incoming_page_url(url,sizeof(url),NULL,1,0)==0 && !strcmp(url,"/sms/parts?page=1"));
    CHECK(web_incoming_page_url(url,sizeof(url),NULL,3,1)==0 && !strcmp(url,"/sms/parts.json?page=3"));
    CHECK(web_incoming_page_url(url,sizeof(url),"sim-A",2,0)==0 && !strcmp(url,"/sms/parts/sim-A?page=2"));
    CHECK(web_incoming_page_url(url,sizeof(url),"sim-A",4,1)==0 && !strcmp(url,"/sms/parts/sim-A.json?page=4"));
    CHECK(web_incoming_page_url(url,sizeof(url),NULL,0,0)==-1);
    CHECK(web_incoming_page_url(url,4,NULL,1,0)==-1);
    CHECK(web_incoming_page_url(NULL,sizeof(url),NULL,1,0)==-1);
    CHECK(web_incoming_page_url(url,sizeof(url),NULL,1,2)==0 && !strcmp(url,"/sms/parts.json?page=1"));
    CHECK(web_incoming_page_url(url,sizeof(url),"sim-A",INT_MAX,0)==0 && strstr(url,"page=2147483647") != NULL);
    CHECK(web_incoming_page_url(url,sizeof(url),long_path,1,0)==-1);
    CHECK(url[sizeof(url)-1] == '\0');
    printf("incoming parts web seam: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
