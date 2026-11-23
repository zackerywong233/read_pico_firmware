/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：xml_tok 与 feeds_store 宿主机回归测试：覆盖流式 XML 分词、实体解码、
 * DOCTYPE 安全跳过、RSS 2.0 / Atom 1.0 / OPDS 1.2 解析、相对 URL 解析、
 * 凭据脱敏、Basic Auth 编码与离线缓存读写。
 * English: Host regression tests for xml_tok and feeds_store: streaming XML tokenization,
 * entity decoding, safe DOCTYPE skipping, RSS 2.0 / Atom 1.0 / OPDS 1.2 parsing,
 * relative URL resolution, credential redaction, Basic Auth encoding, and offline cache.
 */
#include "feeds_store.h"
#include "xml_tok.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_xml_tok(void) {
    const char *doc =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE feed SYSTEM \"http://evil.example/dtd\">\n"
        "<!-- comment -->\n"
        "<feed xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title attr=\"a&amp;b\">Hello &mdash; &#20013;&#x6587; &lt;1&gt;</dc:title>"
        "<body foo='bar'><![CDATA[<raw>&amp;text]]></body>"
        "</feed>";
    xml_reader_t r;
    xml_reader_init(&r, doc, strlen(doc), false);
    xml_tok_t t;
    int seen_title = 0;
    int seen_cdata = 0;
    while (xml_reader_next(&r, &t)) {
        if (t.kind == XML_TOK_OPEN && xml_span_eq(xml_local_name(t.name), "title")) {
            char val[64] = {0};
            assert(xml_tok_attr(&t, "attr", val, sizeof(val), false));
            assert(strcmp(val, "a&b") == 0);
            seen_title = 1;
        } else if (t.kind == XML_TOK_TEXT && seen_title == 1) {
            char buf[128] = {0};
            xml_decode_span(t.text, buf, sizeof(buf), true, t.cdata);
            assert(strstr(buf, "Hello") != NULL);
            assert(strstr(buf, "中文") != NULL);
            assert(strstr(buf, "<1>") != NULL);
            seen_title = 2;
        } else if (t.kind == XML_TOK_TEXT && t.cdata) {
            char buf[64] = {0};
            xml_decode_span(t.text, buf, sizeof(buf), false, t.cdata);
            assert(strcmp(buf, "<raw>&amp;text") == 0);
            seen_cdata = 1;
        }
    }
    assert(!r.failed);
    assert(seen_title == 2);
    assert(seen_cdata == 1);

    // Strict mode must reject unbalanced tags.
    const char *bad = "<a><b></a>";
    xml_reader_init(&r, bad, strlen(bad), false);
    while (xml_reader_next(&r, &t)) {}
    assert(r.failed);
}

static void test_rss2_parse(void) {
    const char *rss =
        "<?xml version=\"1.0\"?>\n"
        "<rss version=\"2.0\" xmlns:content=\"http://purl.org/rss/1.0/modules/content/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "<channel>\n"
        "  <title>技术周刊 &amp; 笔记</title>\n"
        "  <item>\n"
        "    <title>第一篇：墨水屏排版</title>\n"
        "    <link>/posts/1</link>\n"
        "    <dc:creator>作者甲</dc:creator>\n"
        "    <description>简短摘要</description>\n"
        "    <content:encoded><![CDATA[<p>这是<b>完整正文</b>段落。</p>]]></content:encoded>\n"
        "  </item>\n"
        "  <item>\n"
        "    <title>第二篇：只有摘要</title>\n"
        "    <link>https://example.com/posts/2</link>\n"
        "    <description>仅提供摘要内容</description>\n"
        "  </item>\n"
        "</channel>\n"
        "</rss>";
    feed_parsed_t p;
    assert(feeds_parse_xml(rss, strlen(rss), "https://blog.example.com/rss.xml", &p, 16));
    assert(p.doc_kind == FEED_DOC_RSS);
    assert(strcmp(p.title, "技术周刊 & 笔记") == 0);
    assert(p.count == 2);
    assert(strcmp(p.items[0].title, "第一篇：墨水屏排版") == 0);
    assert(strcmp(p.items[0].author, "作者甲") == 0);
    assert(strcmp(p.items[0].link, "https://blog.example.com/posts/1") == 0);
    assert(p.items[0].content_html != NULL);
    assert(strstr(p.items[0].content_html, "完整正文") != NULL);
    assert(strcmp(p.items[1].title, "第二篇：只有摘要") == 0);
    assert(p.items[1].content_html != NULL);
    assert(strstr(p.items[1].content_html, "仅提供摘要内容") != NULL);
    feeds_parsed_free(&p);
}

static void test_opds12_parse(void) {
    const char *opds =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
        "  <title>我的书库</title>\n"
        "  <link rel=\"self\" type=\"application/atom+xml;profile=opds-catalog\" href=\"/opds/index.xml\"/>\n"
        "  <link rel=\"next\" type=\"application/atom+xml;profile=opds-catalog\" href=\"page2.xml\"/>\n"
        "  <link rel=\"search\" type=\"application/opensearchdescription+xml\" href=\"/opds/search.xml\"/>\n"
        "  <entry>\n"
        "    <title>科幻分类</title>\n"
        "    <link rel=\"subsection\" type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\" href=\"/opds/scifi.xml\"/>\n"
        "  </entry>\n"
        "  <entry>\n"
        "    <title>三体全集</title>\n"
        "    <author><name>刘慈欣</name></author>\n"
        "    <summary>地球往事三部曲</summary>\n"
        "    <link rel=\"http://opds-spec.org/acquisition\" type=\"text/plain\" href=\"/dl/1.txt\"/>\n"
        "    <link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" href=\"/dl/1.epub\"/>\n"
        "  </entry>\n"
        "  <entry>\n"
        "    <title>漫画卷一</title>\n"
        "    <link rel=\"http://opds-spec.org/acquisition\" type=\"application/vnd.comicbook+zip\" href=\"comics/v1.cbz\"/>\n"
        "  </entry>\n"
        "</feed>";
    feed_parsed_t p;
    assert(feeds_parse_xml(opds, strlen(opds), "https://lib.example.com/opds/index.xml", &p, 16));
    assert(p.doc_kind == FEED_DOC_OPDS);
    assert(strcmp(p.title, "我的书库") == 0);
    assert(strcmp(p.next_url, "https://lib.example.com/opds/page2.xml") == 0);
    assert(strcmp(p.search_url, "https://lib.example.com/opds/search.xml") == 0);
    assert(p.count == 3);

    // Entry 0: navigation subsection.
    assert(p.items[0].is_nav == true);
    assert(strcmp(p.items[0].link, "https://lib.example.com/opds/scifi.xml") == 0);

    // Entry 1: must pick epub over txt due to format priority.
    assert(p.items[1].is_nav == false);
    assert(strcmp(p.items[1].author, "刘慈欣") == 0);
    assert(strcmp(p.items[1].acq_ext, "epub") == 0);
    assert(strcmp(p.items[1].acq_url, "https://lib.example.com/dl/1.epub") == 0);

    // Entry 2: CBZ acquisition with relative URL resolution.
    assert(strcmp(p.items[2].acq_ext, "cbz") == 0);
    assert(strcmp(p.items[2].acq_url, "https://lib.example.com/opds/comics/v1.cbz") == 0);

    feeds_parsed_free(&p);
}

static void test_store_redaction_and_cache(void) {
    assert(feeds_url_valid("https://example.com/opds"));
    assert(feeds_url_valid("http://192.168.1.5:8080/feed.xml"));
    assert(!feeds_url_valid("ftp://example.com/a"));
    assert(!feeds_url_valid("https://bad url/a"));
    assert(!feeds_url_valid("https://"));

    char auth[128] = {0};
    assert(feeds_build_basic_auth("Aladdin", "open sesame", auth, sizeof(auth)));
    assert(strcmp(auth, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==") == 0);

    bool full = false;
    assert(feeds_store_upsert("书库A", "https://lib.example.com/opds", FEED_KIND_OPDS,
                              "reader", "s3cr3t!", false, &full));
    assert(!full);

    // Public list must redact password while reporting has_password = true.
    feed_public_source_t pub[FEEDS_MAX_SOURCES];
    size_t n = feeds_store_list_public(pub, FEEDS_MAX_SOURCES);
    assert(n == 1);
    assert(strcmp(pub[0].name, "书库A") == 0);
    assert(strcmp(pub[0].auth_user, "reader") == 0);
    assert(pub[0].has_password == true);

    // Upsert with empty password preserves existing password.
    assert(feeds_store_upsert("书库A改名", "https://lib.example.com/opds", FEED_KIND_OPDS,
                              "reader", "", false, &full));
    feed_source_t raw[FEEDS_MAX_SOURCES];
    size_t raw_n = 0;
    assert(feeds_store_load(raw, FEEDS_MAX_SOURCES, &raw_n));
    assert(raw_n == 1);
    assert(strcmp(raw[0].name, "书库A改名") == 0);
    assert(strcmp(raw[0].auth_pass, "s3cr3t!") == 0);

    // Clear auth wipes username and password.
    assert(feeds_store_upsert("书库A改名", "https://lib.example.com/opds", FEED_KIND_OPDS,
                              "", "", true, &full));
    n = feeds_store_list_public(pub, FEEDS_MAX_SOURCES);
    assert(n == 1 && !pub[0].has_password && pub[0].auth_user[0] == '\0');

    // Delete source.
    bool found = false;
    assert(feeds_store_delete("https://lib.example.com/opds", &found));
    assert(found);
    assert(feeds_store_list_public(pub, FEEDS_MAX_SOURCES) == 0);

    // Offline cache round-trip.
    char tmpl[] = "/tmp/pico_feeds_cache_XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root != NULL);
    const char *sample = "<rss><channel><title>Offline</title></channel></rss>";
    assert(feeds_cache_write(root, "https://blog.example.com/rss", sample, strlen(sample)));
    char *loaded = NULL;
    size_t loaded_len = 0;
    assert(feeds_cache_read(root, "https://blog.example.com/rss", &loaded, &loaded_len));
    assert(loaded_len == strlen(sample));
    assert(strcmp(loaded, sample) == 0);
    free(loaded);

    char cpath[360];
    assert(feeds_cache_path(root, "https://blog.example.com/rss", cpath, sizeof(cpath)));
    unlink(cpath);
    char fdir[320];
    snprintf(fdir, sizeof(fdir), "%s/feeds", root);
    rmdir(fdir);
    rmdir(root);
}

int main(void) {
    test_xml_tok();
    test_rss2_parse();
    test_opds12_parse();
    test_store_redaction_and_cache();
    printf("feeds_xml_host_test: OK\n");
    return 0;
}
