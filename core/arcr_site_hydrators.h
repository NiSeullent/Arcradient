/*
 * Waterscape Browser 2.0 / Arcradient Engine (MSHTML-Next)
 * core/arcr_site_hydrators.h - Modern Web App Compatibility & Native Site Hydrators
 *
 * Included exclusively by `core/arcr_engine.c`.
 * Provides:
 *   1. Google Search (`google.com` / `google.co.kr`) search-box & layout normalization
 *   2. Next.js App Router / React 19 (`__next_f`, `www.zuzunza.com`) SSR snapshot & wipeout recovery
 *   3. YouTube (`youtube.com` / `youtu.be`) `ytInitialData` + `ytInitialPlayerResponse` Cinema Hydrator
 *   4. X / Twitter (`x.com` / `twitter.com`) Syndication Post Hydrator & Explore View
 */
#ifndef ARCR_SITE_HYDRATORS_H
#define ARCR_SITE_HYDRATORS_H

/* -------------------------------------------------------------------------
 * 1. Bounded Substring, URL Query, HTML-Escape & JSON Extraction Helpers
 * ------------------------------------------------------------------------- */
static const char *hydrator_memmem_str(const char *hay, size_t hay_len, const char *needle) {
    if (!hay || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0u) return hay;
    if (hay_len < nlen) return NULL;
    const char *limit = hay + (hay_len - nlen);
    char first = needle[0];
    for (const char *p = hay; p <= limit; ++p) {
        if (*p == first && memcmp(p, needle, nlen) == 0) {
            return p;
        }
    }
    return NULL;
}

static bool hydrator_host_matches(const char *host, const char *domain) {
    if (!host || !domain || host[0] == '\0' || domain[0] == '\0') return false;
    size_t hlen = strlen(host);
    size_t dlen = strlen(domain);
    if (hlen == dlen) {
        return arcr_str_eq_nocase(host, domain);
    }
    if (hlen > dlen && host[hlen - dlen - 1u] == '.') {
        return arcr_str_eq_nocase(host + (hlen - dlen), domain);
    }
    return false;
}

static int hydrator_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static void hydrator_append_html_escaped(EngineStrBuf *sb, const char *s) {
    if (!sb || !s) return;
    for (size_t i = 0u; s[i] != '\0'; ++i) {
        char c = s[i];
        if (c == '&')      (void)strbuf_append_cstr(sb, "&amp;");
        else if (c == '<') (void)strbuf_append_cstr(sb, "&lt;");
        else if (c == '>') (void)strbuf_append_cstr(sb, "&gt;");
        else if (c == '"') (void)strbuf_append_cstr(sb, "&quot;");
        else if (c == '\'')(void)strbuf_append_cstr(sb, "&#39;");
        else               (void)strbuf_append_char(sb, c);
    }
}

static void hydrator_extract_query_param(const char *url, const char *key,
                                         char *out_val, size_t out_cap) {
    if (!out_val || out_cap == 0u) return;
    out_val[0] = '\0';
    if (!url || !key || key[0] == '\0') return;

    const char *q = strchr(url, '?');
    if (!q) return;
    ++q;
    size_t klen = strlen(key);

    while (*q != '\0' && *q != '#') {
        const char *amp = q;
        while (*amp != '\0' && *amp != '&' && *amp != '#') ++amp;
        const char *eq = memchr(q, '=', (size_t)(amp - q));
        if (eq && (size_t)(eq - q) == klen && strncmp(q, key, klen) == 0) {
            const char *v = eq + 1;
            size_t pos = 0u;
            while (v < amp && pos + 1u < out_cap) {
                if (*v == '+') {
                    out_val[pos++] = ' ';
                    ++v;
                } else if (*v == '%' && (v + 2) < amp) {
                    int hi = hydrator_hex_nibble(v[1]);
                    int lo = hydrator_hex_nibble(v[2]);
                    if (hi >= 0 && lo >= 0) {
                        out_val[pos++] = (char)((hi << 4) | lo);
                        v += 3;
                        continue;
                    }
                    out_val[pos++] = *v++;
                } else {
                    out_val[pos++] = *v++;
                }
            }
            out_val[pos] = '\0';
            return;
        }
        if (*amp == '&') q = amp + 1;
        else break;
    }
}

/* Finds balanced `{ ... }` starting at or shortly after `start` within `[start, end)` */
static const char *hydrator_balance_braces(const char *start, const char *end, size_t *out_len) {
    if (out_len) *out_len = 0u;
    if (!start || start >= end) return NULL;

    const char *p = start;
    while (p < end && *p != '{') {
        if ((size_t)(p - start) > 160u) return NULL;
        ++p;
    }
    if (p >= end || *p != '{') return NULL;

    const char *obj_start = p;
    int  depth  = 0;
    bool in_str = false;
    bool esc    = false;

    for (; p < end; ++p) {
        char c = *p;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                if (out_len) *out_len = (size_t)((p - obj_start) + 1);
                return obj_start;
            }
        }
    }
    return NULL;
}

static const char *hydrator_extract_json_var(const char *html, size_t html_len,
                                             const char *marker, size_t *out_len) {
    if (out_len) *out_len = 0u;
    const char *hit = hydrator_memmem_str(html, html_len, marker);
    if (!hit) return NULL;
    const char *after = hit + strlen(marker);
    return hydrator_balance_braces(after, html + html_len, out_len);
}

/* Unescapes a JSON string starting right after the opening `"` */
static size_t hydrator_unescape_json_str(const char *p, const char *end,
                                         char *out, size_t out_cap,
                                         const char **out_after) {
    size_t pos = 0u;
    while (p < end && *p != '"') {
        if (*p == '\\' && (p + 1) < end) {
            ++p;
            char e = *p++;
            char mapped = 0;
            if (e == '"' || e == '\\' || e == '/') mapped = e;
            else if (e == 'n') mapped = '\n';
            else if (e == 'r') mapped = '\r';
            else if (e == 't') mapped = '\t';
            else if (e == 'b') mapped = '\b';
            else if (e == 'f') mapped = '\f';
            else if (e == 'u' && (p + 4) <= end) {
                int h0 = hydrator_hex_nibble(p[0]);
                int h1 = hydrator_hex_nibble(p[1]);
                int h2 = hydrator_hex_nibble(p[2]);
                int h3 = hydrator_hex_nibble(p[3]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    uint32_t cp = (uint32_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    p += 4;
                    /* UTF-16 surrogate pair handling */
                    if (cp >= 0xD800u && cp <= 0xDBFFu && (p + 6) <= end &&
                        p[0] == '\\' && p[1] == 'u') {
                        int l0 = hydrator_hex_nibble(p[2]);
                        int l1 = hydrator_hex_nibble(p[3]);
                        int l2 = hydrator_hex_nibble(p[4]);
                        int l3 = hydrator_hex_nibble(p[5]);
                        if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                            uint32_t lo = (uint32_t)((l0 << 12) | (l1 << 8) | (l2 << 4) | l3);
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                                p += 6;
                            }
                        }
                    }
                    char u8[4];
                    size_t un = arcr_utf8_encode_one(cp, u8);
                    if (out && pos + un < out_cap) {
                        memcpy(out + pos, u8, un);
                        pos += un;
                    }
                    continue;
                }
            }
            if (mapped != 0 && out && pos + 1u < out_cap) {
                out[pos++] = mapped;
            }
            continue;
        }
        if (out && pos + 1u < out_cap) {
            out[pos++] = *p;
        }
        ++p;
    }
    if (p < end && *p == '"') ++p;
    if (out && out_cap > 0u) out[pos] = '\0';
    if (out_after) *out_after = p;
    return pos;
}

static bool hydrator_json_get_str(const char *json, size_t json_len, const char *key,
                                  char *out_val, size_t val_cap) {
    if (out_val && val_cap > 0u) out_val[0] = '\0';
    if (!json || json_len == 0u || !key) return false;

    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t plen = strlen(pat);
    const char *end = json + json_len;
    const char *cur = json;

    while (cur < end) {
        const char *hit = hydrator_memmem_str(cur, (size_t)(end - cur), pat);
        if (!hit) return false;
        const char *p = hit + plen;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (p < end && *p == ':') {
            ++p;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
            if (p < end && *p == '"') {
                hydrator_unescape_json_str(p + 1, end, out_val, val_cap, NULL);
                return true;
            }
            if (p < end && ((*p >= '0' && *p <= '9') || *p == '-')) {
                size_t k = 0u;
                while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') &&
                       k + 1u < val_cap) {
                    out_val[k++] = *p++;
                }
                if (val_cap > 0u) out_val[k] = '\0';
                return k > 0u;
            }
        }
        cur = hit + plen;
    }
    return false;
}

/* Extracts YouTube text field supporting simpleText, runs[0..N].text, or plain string */
static bool hydrator_yt_get_text(const char *obj, size_t obj_len, const char *field,
                                 char *out_val, size_t val_cap) {
    if (out_val && val_cap > 0u) out_val[0] = '\0';
    if (!obj || obj_len == 0u || !field) return false;

    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", field);
    const char *end = obj + obj_len;
    const char *hit = hydrator_memmem_str(obj, obj_len, pat);
    if (!hit) return false;

    const char *p = hit + strlen(pat);
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p != ':') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end) return false;

    if (*p == '"') {
        hydrator_unescape_json_str(p + 1, end, out_val, val_cap, NULL);
        return out_val[0] != '\0';
    }
    if (*p == '{') {
        size_t sub_len = 0u;
        const char *sub = hydrator_balance_braces(p, end, &sub_len);
        if (!sub || sub_len == 0u) return false;
        if (hydrator_json_get_str(sub, sub_len, "simpleText", out_val, val_cap) &&
            out_val[0] != '\0') {
            return true;
        }
        /* Concatenate runs[].text */
        const char *runs = hydrator_memmem_str(sub, sub_len, "\"runs\"");
        if (runs) {
            const char *rcur = runs + 6;
            const char *rend = sub + sub_len;
            size_t total = 0u;
            while (rcur < rend && total + 1u < val_cap) {
                const char *tx = hydrator_memmem_str(rcur, (size_t)(rend - rcur), "\"text\"");
                if (!tx) break;
                const char *q = tx + 6;
                while (q < rend && (*q == ' ' || *q == ':')) ++q;
                if (q < rend && *q == '"') {
                    const char *after = NULL;
                    size_t n = hydrator_unescape_json_str(q + 1, rend,
                                                          out_val + total, val_cap - total, &after);
                    total += n;
                    rcur = after ? after : (q + 1);
                } else {
                    rcur = tx + 6;
                }
            }
            if (val_cap > 0u) out_val[total] = '\0';
            return total > 0u;
        }
    }
    return false;
}

/* Counts non-whitespace visible characters and rich interactive/media elements */
static void hydrator_count_visible_dom(const ArcrDocument *doc, const ArcrDomNode *n,
                                       size_t *io_text_chars, size_t *io_rich_els) {
    if (!doc || !n) return;
    if (n->kind == ARCR_NODE_TEXT) {
        if (n->text && n->text_len > 0u) {
            for (size_t i = 0u; i < n->text_len; ++i) {
                unsigned char c = (unsigned char)n->text[i];
                if (c > 0x20u) (*io_text_chars)++;
            }
        }
        return;
    }
    if (n->kind != ARCR_NODE_ELEMENT) return;

    const char *tag = arcr_atom_cstr(doc->atoms, n->tag);
    if (arcr_str_eq_nocase(tag, "script") ||
        arcr_str_eq_nocase(tag, "style") ||
        arcr_str_eq_nocase(tag, "noscript") ||
        arcr_str_eq_nocase(tag, "template") ||
        arcr_str_eq_nocase(tag, "head") ||
        arcr_str_eq_nocase(tag, "meta") ||
        arcr_str_eq_nocase(tag, "link")) {
        return;
    }
    if (arcr_dom_get_attr_cstr(doc, n, "hidden") != NULL) return;

    if (arcr_str_eq_nocase(tag, "video") ||
        arcr_str_eq_nocase(tag, "canvas") ||
        arcr_str_eq_nocase(tag, "img") ||
        arcr_str_eq_nocase(tag, "svg") ||
        arcr_str_eq_nocase(tag, "textarea")) {
        (*io_rich_els)++;
    } else if (arcr_str_eq_nocase(tag, "input")) {
        const char *tp = arcr_dom_get_attr_cstr(doc, n, "type");
        if (!tp || !arcr_str_eq_nocase(tp, "hidden")) {
            (*io_rich_els)++;
        }
    }

    for (const ArcrDomNode *ch = n->first_child; ch != NULL; ch = ch->next_sibling) {
        hydrator_count_visible_dom(doc, ch, io_text_chars, io_rich_els);
    }
}

/* -------------------------------------------------------------------------
 * 2. Pre-Script Hooks: <noscript> Hide, Google CSS & Next.js SSR Snapshot
 * ------------------------------------------------------------------------- */
static void arcr_hydrator_before_scripts(ArcrWebView *view,
                                         const char  *raw_html,
                                         size_t       raw_len,
                                         char       **out_ssr_snapshot,
                                         size_t      *out_ssr_len,
                                         bool        *out_needs_nextjs_shim) {
    if (out_ssr_snapshot)      *out_ssr_snapshot      = NULL;
    if (out_ssr_len)           *out_ssr_len           = 0u;
    if (out_needs_nextjs_shim) *out_needs_nextjs_shim = false;
    if (!view || !view->doc || !view->engine) return;
    if (view->engine->cfg.conformance_mode) return;

    ArcrDocument *doc = view->doc;
    char host[192];
    extract_url_host(view->url, host, sizeof(host));

    /* Hide <noscript> fallback banners when JavaScript is enabled */
    if (view->engine->cfg.enable_javascript) {
        ArcrDomNode *ns_hit = NULL;
        if (arcr_dom_query_selector_all(doc, doc->root, "noscript", &ns_hit, 1u) > 0u) {
            const char *ns_css = "noscript { display: none !important; }\n";
            arcr_dom_append_style_block(doc, ns_css, strlen(ns_css));
        }
    }

    /* Google Search (`google.com` / `google.co.kr`) search bar & button normalization */
    if (hydrator_host_matches(host, "google.com") ||
        hydrator_host_matches(host, "google.co.kr")) {
        const char *g_css =
            "center { display: block; text-align: center; }\n"
            "form[action*=\"/search\"] { max-width: 600px; margin: 16px auto; }\n"
            "textarea[name=\"q\"], input[name=\"q\"] {\n"
            "  display: block; width: 100%; max-width: 584px; height: 44px; min-height: 44px;\n"
            "  margin: 8px auto; padding: 10px 18px; font-size: 16px; line-height: 24px;\n"
            "  border: 1px solid #dfe1e5; border-radius: 24px; box-sizing: border-box;\n"
            "  background: #ffffff; color: #202124;\n"
            "}\n";
        arcr_dom_append_style_block(doc, g_css, strlen(g_css));
    }

    /* Next.js App Router / React 19 (`__next_f` / `_next/` / `zuzunza.com`) SSR protection */
    bool is_nextjs_or_zuzunza =
        hydrator_host_matches(host, "zuzunza.com") ||
        (raw_html && raw_len > 0u &&
         (hydrator_memmem_str(raw_html, raw_len, "__next_f") != NULL ||
          hydrator_memmem_str(raw_html, raw_len, "_next/static") != NULL ||
          hydrator_memmem_str(raw_html, raw_len, "__NEXT_DATA__") != NULL));

    if (is_nextjs_or_zuzunza && view->engine->cfg.enable_javascript && doc->body) {
        if (out_needs_nextjs_shim) *out_needs_nextjs_shim = true;
        size_t ssr_text = 0u, ssr_rich = 0u;
        hydrator_count_visible_dom(doc, doc->body, &ssr_text, &ssr_rich);
        if ((ssr_text >= 24u || ssr_rich >= 2u) && out_ssr_snapshot && out_ssr_len) {
            *out_ssr_snapshot = arcr_dom_serialize_html_alloc(doc, doc->body, false, out_ssr_len);
        }
    }
}

/* -------------------------------------------------------------------------
 * 3. YouTube (`ytInitialData` + `ytInitialPlayerResponse`) Hydrator
 * ------------------------------------------------------------------------- */
typedef struct YtHydratedCard {
    char video_id[24];
    char title[192];
    char channel[96];
    char views[64];
    char duration[32];
} YtHydratedCard;

#define ARCR_MAX_YT_CARDS 24u

static void hydrator_extract_yt_watch_id(const char *url, char *out_id, size_t cap) {
    if (!out_id || cap == 0u) return;
    out_id[0] = '\0';
    if (!url) return;

    hydrator_extract_query_param(url, "v", out_id, cap);
    if (out_id[0] != '\0') return;

    const char *yb = strstr(url, "youtu.be/");
    const char *sh = strstr(url, "/shorts/");
    const char *em = strstr(url, "/embed/");
    const char *seg = yb ? (yb + 9) : (sh ? (sh + 8) : (em ? (em + 7) : NULL));
    if (seg) {
        size_t i = 0u;
        while (seg[i] != '\0' && seg[i] != '?' && seg[i] != '&' &&
               seg[i] != '/' && seg[i] != '#' && i + 1u < cap) {
            out_id[i] = seg[i];
            ++i;
        }
        out_id[i] = '\0';
    }
}

static size_t hydrator_scan_yt_cards(const char *yt_json, size_t yt_len,
                                     const char *skip_vid,
                                     YtHydratedCard *out_cards, size_t max_cards) {
    if (!yt_json || yt_len == 0u || !out_cards || max_cards == 0u) return 0u;

    static const char *const kRendererKeys[] = {
        "\"videoRenderer\"",
        "\"gridVideoRenderer\"",
        "\"compactVideoRenderer\"",
        "\"reelItemRenderer\""
    };

    size_t count = 0u;
    const char *end = yt_json + yt_len;

    for (size_t k = 0u; k < ARCR_ARRAY_LEN(kRendererKeys) && count < max_cards; ++k) {
        const char *key = kRendererKeys[k];
        size_t klen = strlen(key);
        const char *cur = yt_json;

        while (cur < end && count < max_cards) {
            const char *hit = hydrator_memmem_str(cur, (size_t)(end - cur), key);
            if (!hit) break;
            size_t rlen = 0u;
            const char *robj = hydrator_balance_braces(hit + klen, end, &rlen);
            if (!robj || rlen == 0u) {
                cur = hit + klen;
                continue;
            }

            char vid[24] = {0};
            if (hydrator_json_get_str(robj, rlen, "videoId", vid, sizeof(vid)) &&
                strlen(vid) >= 8u && (!skip_vid || strcmp(vid, skip_vid) != 0)) {
                bool dup = false;
                for (size_t d = 0u; d < count; ++d) {
                    if (strcmp(out_cards[d].video_id, vid) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    YtHydratedCard *c = &out_cards[count];
                    memset(c, 0, sizeof(*c));
                    arcr_str_copy(c->video_id, sizeof(c->video_id), vid);
                    if (!hydrator_yt_get_text(robj, rlen, "title", c->title, sizeof(c->title))) {
                        (void)hydrator_yt_get_text(robj, rlen, "headline", c->title, sizeof(c->title));
                    }
                    if (!hydrator_yt_get_text(robj, rlen, "ownerText", c->channel, sizeof(c->channel)) &&
                        !hydrator_yt_get_text(robj, rlen, "longBylineText", c->channel, sizeof(c->channel))) {
                        (void)hydrator_yt_get_text(robj, rlen, "shortBylineText", c->channel, sizeof(c->channel));
                    }
                    if (!hydrator_yt_get_text(robj, rlen, "shortViewCountText", c->views, sizeof(c->views))) {
                        (void)hydrator_yt_get_text(robj, rlen, "viewCountText", c->views, sizeof(c->views));
                    }
                    (void)hydrator_yt_get_text(robj, rlen, "lengthText", c->duration, sizeof(c->duration));
                    if (c->title[0] != '\0') {
                        ++count;
                    }
                }
            }
            cur = robj + rlen;
        }
    }
    return count;
}

static bool hydrator_hydrate_youtube(ArcrWebView *view, const char *raw_html, size_t raw_len) {
    if (!view || !view->doc || !view->doc->body) return false;
    ArcrDocument *doc = view->doc;

    /* Skip if page already rendered native <video> */
    ArcrDomNode *existing_vid = NULL;
    if (arcr_dom_query_selector_all(doc, doc->body, "video", &existing_vid, 1u) > 0u) {
        return false;
    }

    char watch_id[32] = {0};
    hydrator_extract_yt_watch_id(view->url, watch_id, sizeof(watch_id));

    size_t init_len = 0u, player_len = 0u;
    const char *yt_init = raw_html ? hydrator_extract_json_var(raw_html, raw_len, "ytInitialData", &init_len) : NULL;
    const char *yt_play = raw_html ? hydrator_extract_json_var(raw_html, raw_len, "ytInitialPlayerResponse", &player_len) : NULL;

    char watch_title[256]  = {0};
    char watch_author[128] = {0};
    char watch_views[64]   = {0};
    char watch_desc[600]   = {0};
    char stream_url[1024]  = {0};

    if (yt_play && player_len > 0u) {
        const char *vd_hit = hydrator_memmem_str(yt_play, player_len, "\"videoDetails\"");
        size_t vd_len = 0u;
        const char *vd = vd_hit ? hydrator_balance_braces(vd_hit + 14, yt_play + player_len, &vd_len) : NULL;
        if (vd && vd_len > 0u) {
            if (watch_id[0] == '\0') {
                (void)hydrator_json_get_str(vd, vd_len, "videoId", watch_id, sizeof(watch_id));
            }
            (void)hydrator_json_get_str(vd, vd_len, "title", watch_title, sizeof(watch_title));
            (void)hydrator_json_get_str(vd, vd_len, "author", watch_author, sizeof(watch_author));
            (void)hydrator_json_get_str(vd, vd_len, "viewCount", watch_views, sizeof(watch_views));
            (void)hydrator_json_get_str(vd, vd_len, "shortDescription", watch_desc, sizeof(watch_desc));
        }

        /* Probe unencrypted progressive MP4 stream URL in streamingData.formats */
        const char *sd_hit = hydrator_memmem_str(yt_play, player_len, "\"formats\"");
        if (sd_hit) {
            size_t rem = (size_t)((yt_play + player_len) - sd_hit);
            (void)hydrator_json_get_str(sd_hit, rem, "url", stream_url, sizeof(stream_url));
        }
    }

    YtHydratedCard *cards = (YtHydratedCard *)calloc(ARCR_MAX_YT_CARDS, sizeof(YtHydratedCard));
    size_t card_count = 0u;
    if (cards && yt_init && init_len > 0u) {
        card_count = hydrator_scan_yt_cards(yt_init, init_len,
                                            watch_id[0] ? watch_id : NULL,
                                            cards, ARCR_MAX_YT_CARDS);
    }

    /* If neither watch_id nor extracted cards exist and page already has plenty of text, leave untouched */
    size_t vis_text = 0u, vis_rich = 0u;
    hydrator_count_visible_dom(doc, doc->body, &vis_text, &vis_rich);
    if (watch_id[0] == '\0' && card_count == 0u && vis_text >= 120u) {
        free(cards);
        return false;
    }

    const char *yt_css_1 =
        "body { margin:0; padding:0; background:#0f0f0f; color:#f1f1f1; font-family:sans-serif; }\n"
        ".yt-hdr { position:sticky; top:0; z-index:50; display:flex; align-items:center; justify-content:space-between;"
        "  padding:10px 20px; background:#0f0f0f; border-bottom:1px solid #272727; gap:16px; }\n"
        ".yt-brand { display:flex; align-items:center; gap:8px; color:#ffffff; text-decoration:none; font-size:18px; font-weight:bold; }\n"
        ".yt-badge { background:#ff0000; color:#ffffff; padding:3px 9px; border-radius:6px; font-size:13px; font-weight:bold; }\n"
        ".yt-sform { display:flex; flex:1; max-width:560px; gap:0; }\n"
        ".yt-sinp { flex:1; height:36px; padding:6px 14px; background:#121212; color:#f1f1f1; border:1px solid #303030;"
        "  border-radius:18px 0 0 18px; font-size:14px; box-sizing:border-box; }\n"
        ".yt-sbtn { height:36px; padding:0 18px; background:#222222; color:#f1f1f1; border:1px solid #303030;"
        "  border-radius:0 18px 18px 0; font-size:13px; }\n";
    const char *yt_css_2 =
        ".yt-chips { display:flex; gap:8px; padding:10px 20px; flex-wrap:wrap; border-bottom:1px solid #1f1f1f; }\n"
        ".yt-chip { padding:6px 12px; background:#272727; color:#f1f1f1; border-radius:8px; font-size:13px; text-decoration:none; }\n"
        ".yt-chip-on { background:#f1f1f1; color:#0f0f0f; font-weight:bold; }\n"
        ".yt-wrap { padding:20px; max-width:1360px; margin:0 auto; }\n"
        ".yt-grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(260px, 1fr)); gap:18px; }\n"
        ".yt-card { display:block; background:#181818; border:1px solid #262626; border-radius:12px; overflow:hidden; text-decoration:none; color:#f1f1f1; }\n"
        ".yt-card-body { padding:10px 12px; }\n"
        ".yt-ctitle { font-size:14px; font-weight:bold; color:#f1f1f1; margin-bottom:6px; line-height:1.35; }\n"
        ".yt-cmeta { font-size:12px; color:#aaaaaa; }\n"
        ".yt-watch { display:flex; gap:20px; flex-wrap:wrap; align-items:flex-start; }\n"
        ".yt-main { flex:2; min-width:460px; }\n"
        ".yt-side { flex:1; min-width:280px; display:flex; flex-direction:column; gap:12px; }\n"
        ".yt-desc { margin-top:12px; padding:12px 14px; background:#272727; border-radius:10px; font-size:13px; color:#d4d4d4; white-space:pre; }\n";

    arcr_dom_append_style_block(doc, yt_css_1, strlen(yt_css_1));
    arcr_dom_append_style_block(doc, yt_css_2, strlen(yt_css_2));

    char search_q[160] = {0};
    hydrator_extract_query_param(view->url, "search_query", search_q, sizeof(search_q));

    EngineStrBuf html = {0};
    (void)strbuf_reserve(&html, 4096u);

    /* Top Sticky Header */
    (void)strbuf_append_cstr(&html,
        "<div class=\"yt-hdr\">"
        "<a class=\"yt-brand\" href=\"https://www.youtube.com/\">"
        "<span class=\"yt-badge\">&#9654;</span><span>YouTube</span></a>"
        "<form class=\"yt-sform\" action=\"https://www.youtube.com/results\" method=\"get\">"
        "<input class=\"yt-sinp\" type=\"text\" name=\"search_query\" placeholder=\"YouTube 검색\" value=\"");
    hydrator_append_html_escaped(&html, search_q);
    (void)strbuf_append_cstr(&html,
        "\"><button class=\"yt-sbtn\" type=\"submit\">검색</button></form></div>");

    if (watch_id[0] != '\0') {
        if (watch_title[0] == '\0') {
            if (doc->title && doc->title[0] != '\0') {
                arcr_str_copy(watch_title, sizeof(watch_title), doc->title);
            } else {
                snprintf(watch_title, sizeof(watch_title), "YouTube Video (%s)", watch_id);
            }
        }
        if (stream_url[0] == '\0') {
            snprintf(stream_url, sizeof(stream_url), "https://www.youtube.com/watch?v=%s", watch_id);
        }

        char full_title[280];
        snprintf(full_title, sizeof(full_title), "%s - YouTube", watch_title);
        doc->title = arcr_arena_strdup(&doc->arena, full_title, strlen(full_title));
        sync_doc_title_effects(view);

        (void)strbuf_append_cstr(&html, "<div class=\"yt-wrap\"><div class=\"yt-watch\"><div class=\"yt-main\">");
        (void)strbuf_append_cstr(&html, "<video controls autoplay width=\"768\" height=\"432\" "
                                        "style=\"width:100%;height:432px;background:#000000;border-radius:12px;display:block;\" src=\"");
        hydrator_append_html_escaped(&html, stream_url);
        (void)strbuf_append_cstr(&html, "\" title=\"");
        hydrator_append_html_escaped(&html, watch_title);
        (void)strbuf_append_cstr(&html, "\"></video><h1 style=\"font-size:20px;margin:14px 0 8px 0;color:#ffffff;\">");
        hydrator_append_html_escaped(&html, watch_title);
        (void)strbuf_append_cstr(&html, "</h1><div style=\"display:flex;justify-content:space-between;color:#aaaaaa;font-size:13px;\"><span>");
        hydrator_append_html_escaped(&html, watch_author[0] ? watch_author : "YouTube Channel");
        (void)strbuf_append_cstr(&html, "</span><span>");
        if (watch_views[0] != '\0') {
            (void)strbuf_append_cstr(&html, "조회수 ");
            hydrator_append_html_escaped(&html, watch_views);
            (void)strbuf_append_cstr(&html, "회");
        }
        (void)strbuf_append_cstr(&html, "</span></div>");
        if (watch_desc[0] != '\0') {
            (void)strbuf_append_cstr(&html, "<div class=\"yt-desc\">");
            hydrator_append_html_escaped(&html, watch_desc);
            (void)strbuf_append_cstr(&html, "</div>");
        }
        (void)strbuf_append_cstr(&html, "</div><div class=\"yt-side\">"
                                        "<div style=\"font-size:15px;font-weight:bold;margin-bottom:4px;\">관련 동영상</div>");
        for (size_t i = 0u; i < card_count && i < 10u; ++i) {
            const YtHydratedCard *c = &cards[i];
            (void)strbuf_append_cstr(&html, "<a class=\"yt-card\" href=\"https://www.youtube.com/watch?v=");
            hydrator_append_html_escaped(&html, c->video_id);
            (void)strbuf_append_cstr(&html, "\"><div class=\"yt-card-body\"><div class=\"yt-ctitle\">");
            hydrator_append_html_escaped(&html, c->title);
            (void)strbuf_append_cstr(&html, "</div><div class=\"yt-cmeta\">");
            hydrator_append_html_escaped(&html, c->channel);
            if (c->duration[0] != '\0') {
                (void)strbuf_append_cstr(&html, " · ");
                hydrator_append_html_escaped(&html, c->duration);
            }
            (void)strbuf_append_cstr(&html, "</div></div></a>");
        }
        (void)strbuf_append_cstr(&html, "</div></div></div>");
    } else {
        (void)strbuf_append_cstr(&html,
            "<div class=\"yt-chips\">"
            "<a class=\"yt-chip yt-chip-on\" href=\"https://www.youtube.com/\">전체</a>"
            "<a class=\"yt-chip\" href=\"https://www.youtube.com/results?search_query=음악\">음악</a>"
            "<a class=\"yt-chip\" href=\"https://www.youtube.com/results?search_query=뉴스\">뉴스</a>"
            "<a class=\"yt-chip\" href=\"https://www.youtube.com/results?search_query=게임\">게임</a>"
            "<a class=\"yt-chip\" href=\"https://www.youtube.com/results?search_query=프로그래밍\">프로그래밍</a>"
            "<a class=\"yt-chip\" href=\"https://www.youtube.com/results?search_query=실시간\">실시간</a>"
            "</div><div class=\"yt-wrap\"><div class=\"yt-grid\">");

        for (size_t i = 0u; i < card_count; ++i) {
            const YtHydratedCard *c = &cards[i];
            (void)strbuf_append_cstr(&html, "<a class=\"yt-card\" href=\"https://www.youtube.com/watch?v=");
            hydrator_append_html_escaped(&html, c->video_id);
            (void)strbuf_append_cstr(&html, "\"><img src=\"https://i.ytimg.com/vi/");
            hydrator_append_html_escaped(&html, c->video_id);
            (void)strbuf_append_cstr(&html, "/mqdefault.jpg\" width=\"320\" height=\"180\" "
                                            "style=\"width:100%;height:168px;display:block;background:#222;\">"
                                            "<div class=\"yt-card-body\"><div class=\"yt-ctitle\">");
            hydrator_append_html_escaped(&html, c->title);
            (void)strbuf_append_cstr(&html, "</div><div class=\"yt-cmeta\">");
            hydrator_append_html_escaped(&html, c->channel);
            if (c->views[0] != '\0') {
                (void)strbuf_append_cstr(&html, " · ");
                hydrator_append_html_escaped(&html, c->views);
            }
            if (c->duration[0] != '\0') {
                (void)strbuf_append_cstr(&html, " [");
                hydrator_append_html_escaped(&html, c->duration);
                (void)strbuf_append_cstr(&html, "]");
            }
            (void)strbuf_append_cstr(&html, "</div></div></a>");
        }
        if (card_count == 0u) {
            (void)strbuf_append_cstr(&html,
                "<div style=\"grid-column:span 3;padding:32px;background:#181818;border-radius:12px;\">"
                "<h2 style=\"margin:0 0 8px 0;\">검색어로 시작해 보세요</h2>"
                "<p style=\"color:#aaaaaa;margin:0;\">상단 검색창에 키워드를 입력하면 YouTube 동영상을 즉시 탐색하고 재생합니다.</p>"
                "</div>");
        }
        (void)strbuf_append_cstr(&html, "</div></div>");
    }

    free(cards);

    arcr_dom_clear_children(doc, doc->body);
    (void)arcr_html_parse_fragment(doc, doc->body, html.data ? html.data : "", html.len);
    strbuf_free(&html);

    ArcrAtom in_atom = arcr_atom_intern_lower(&view->engine->atoms, "input", 5u);
    ArcrAtom ta_atom = arcr_atom_intern_lower(&view->engine->atoms, "textarea", 8u);
    init_form_defaults_recursive(doc, doc->body, in_atom, ta_atom);
    fetch_document_subresources(view);
    return true;
}

/* -------------------------------------------------------------------------
 * 4. X / Twitter (`x.com` / `twitter.com`) Syndication Post & Explore Hydrator
 * ------------------------------------------------------------------------- */
static bool hydrator_hydrate_x_twitter(ArcrWebView *view) {
    if (!view || !view->doc || !view->doc->body) return false;
    ArcrDocument *doc = view->doc;

    size_t vis_text = 0u, vis_rich = 0u;
    hydrator_count_visible_dom(doc, doc->body, &vis_text, &vis_rich);
    /* X shells either stay blank (#react-root empty) or show short JS warning */
    if (vis_text > 180u && vis_rich >= 2u) return false;

    const char *st = strstr(view->url, "/status/");
    char tweet_id[40] = {0};
    if (st) {
        st += 8;
        size_t i = 0u;
        while (st[i] >= '0' && st[i] <= '9' && i + 1u < sizeof(tweet_id)) {
            tweet_id[i] = st[i];
            ++i;
        }
        tweet_id[i] = '\0';
    }

    const char *x_css =
        "body { margin:0; padding:0; background:#000000; color:#e7e9ea; font-family:sans-serif; }\n"
        ".x-hdr { display:flex; align-items:center; justify-content:space-between; padding:12px 24px;"
        "  border-bottom:1px solid #2f3336; background:#000000; }\n"
        ".x-logo { font-size:22px; font-weight:bold; color:#ffffff; text-decoration:none; }\n"
        ".x-wrap { max-width:640px; margin:24px auto; padding:0 16px; }\n"
        ".x-card { background:#16181c; border:1px solid #2f3336; border-radius:16px; padding:20px; margin-bottom:16px; }\n"
        ".x-user { font-size:16px; font-weight:bold; color:#ffffff; }\n"
        ".x-sub  { font-size:13px; color:#71767b; margin-bottom:12px; }\n"
        ".x-text { font-size:16px; line-height:1.5; color:#e7e9ea; margin-bottom:14px; }\n"
        ".x-stat { font-size:13px; color:#71767b; padding-top:12px; border-top:1px solid #2f3336; }\n"
        ".x-inp  { width:72%; height:36px; padding:6px 12px; background:#000; color:#e7e9ea;"
        "  border:1px solid #2f3336; border-radius:18px; }\n"
        ".x-btn  { height:36px; padding:0 16px; background:#1d9bf0; color:#fff; border:none; border-radius:18px; font-weight:bold; }\n";
    arcr_dom_append_style_block(doc, x_css, strlen(x_css));

    EngineStrBuf html = {0};
    (void)strbuf_reserve(&html, 2048u);
    (void)strbuf_append_cstr(&html,
        "<div class=\"x-hdr\"><a class=\"x-logo\" href=\"https://x.com/\">X</a>"
        "<form action=\"https://x.com/search\" method=\"get\" style=\"display:flex;gap:8px;width:360px;\">"
        "<input class=\"x-inp\" type=\"text\" name=\"q\" placeholder=\"X 검색 또는 @계정\">"
        "<button class=\"x-btn\" type=\"submit\">탐색</button></form></div><div class=\"x-wrap\">");

    bool got_post = false;
    if (tweet_id[0] != '\0') {
        char synd_url[256];
        snprintf(synd_url, sizeof(synd_url),
                 "https://cdn.syndication.twimg.com/tweet-result?id=%s&token=waterscape", tweet_id);
        uint8_t *sbytes = NULL;
        size_t   slen   = 0u;
        if (engine_fetch_resource_ex(view->engine, synd_url, view->url, "GET", NULL, 0u,
                                     true, &sbytes, &slen, NULL, 0u, NULL, NULL, 0u, NULL, NULL) &&
            sbytes && slen > 0u) {
            const char *sj = (const char *)sbytes;
            char text[1024] = {0}, uname[128] = {0}, sname[96] = {0};
            char likes[32] = {0}, replies[32] = {0}, created[64] = {0}, photo[512] = {0};

            (void)hydrator_json_get_str(sj, slen, "text", text, sizeof(text));
            (void)hydrator_json_get_str(sj, slen, "favorite_count", likes, sizeof(likes));
            (void)hydrator_json_get_str(sj, slen, "conversation_count", replies, sizeof(replies));
            (void)hydrator_json_get_str(sj, slen, "created_at", created, sizeof(created));

            const char *u_hit = hydrator_memmem_str(sj, slen, "\"user\"");
            size_t u_len = 0u;
            const char *u_obj = u_hit ? hydrator_balance_braces(u_hit + 6, sj + slen, &u_len) : NULL;
            if (u_obj && u_len > 0u) {
                (void)hydrator_json_get_str(u_obj, u_len, "name", uname, sizeof(uname));
                (void)hydrator_json_get_str(u_obj, u_len, "screen_name", sname, sizeof(sname));
            }
            if (!hydrator_json_get_str(sj, slen, "media_url_https", photo, sizeof(photo))) {
                const char *ph = hydrator_memmem_str(sj, slen, "\"photos\"");
                if (ph) {
                    (void)hydrator_json_get_str(ph, (size_t)((sj + slen) - ph), "url", photo, sizeof(photo));
                }
            }

            if (text[0] != '\0') {
                got_post = true;
                (void)strbuf_append_cstr(&html, "<div class=\"x-card\"><div class=\"x-user\">");
                hydrator_append_html_escaped(&html, uname[0] ? uname : "X User");
                (void)strbuf_append_cstr(&html, "</div><div class=\"x-sub\">@");
                hydrator_append_html_escaped(&html, sname[0] ? sname : "x");
                if (created[0] != '\0') {
                    (void)strbuf_append_cstr(&html, " · ");
                    hydrator_append_html_escaped(&html, created);
                }
                (void)strbuf_append_cstr(&html, "</div><div class=\"x-text\">");
                hydrator_append_html_escaped(&html, text);
                (void)strbuf_append_cstr(&html, "</div>");
                if (photo[0] != '\0') {
                    (void)strbuf_append_cstr(&html, "<img src=\"");
                    hydrator_append_html_escaped(&html, photo);
                    (void)strbuf_append_cstr(&html, "\" width=\"560\" height=\"320\" "
                                                    "style=\"width:100%;border-radius:12px;margin-bottom:12px;display:block;\">");
                }
                (void)strbuf_append_cstr(&html, "<div class=\"x-stat\">답글 ");
                hydrator_append_html_escaped(&html, replies[0] ? replies : "0");
                (void)strbuf_append_cstr(&html, " · 마음에 들어요 ");
                hydrator_append_html_escaped(&html, likes[0] ? likes : "0");
                (void)strbuf_append_cstr(&html, "</div></div>");
            }
        }
        engine_free_resource(view->engine, sbytes);
    }

    if (!got_post) {
        (void)strbuf_append_cstr(&html,
            "<div class=\"x-card\">"
            "<div class=\"x-user\">X (Twitter) 공개 포스트 &amp; 탐색 뷰어</div>"
            "<div class=\"x-sub\">Waterscape 2.0 Smart Syndication Hydrator</div>"
            "<div class=\"x-text\">X 홈 타임라인은 계정 로그인을 요구합니다. 공개 게시물 주소"
            " (예: https://x.com/user/status/20)로 이동하면 로그인 없이 본문과 이미지를 즉시 렌더링합니다.</div>"
            "<div style=\"display:flex;gap:10px;flex-wrap:wrap;\">"
            "<a class=\"x-btn\" style=\"padding:8px 16px;text-decoration:none;display:inline-block;\" "
            "href=\"https://x.com/jack/status/20\">샘플 공개 포스트 보기 (#20)</a>"
            "</div></div>");
    }

    (void)strbuf_append_cstr(&html, "</div>");
    arcr_dom_clear_children(doc, doc->body);
    (void)arcr_html_parse_fragment(doc, doc->body, html.data ? html.data : "", html.len);
    strbuf_free(&html);

    ArcrAtom in_atom = arcr_atom_intern_lower(&view->engine->atoms, "input", 5u);
    ArcrAtom ta_atom = arcr_atom_intern_lower(&view->engine->atoms, "textarea", 8u);
    init_form_defaults_recursive(doc, doc->body, in_atom, ta_atom);
    fetch_document_subresources(view);
    return true;
}

/* -------------------------------------------------------------------------
 * 5. Post-Script Dispatcher: SSR Wipeout Recovery + Site-Specific Hydrators
 * ------------------------------------------------------------------------- */
static bool arcr_hydrator_after_scripts(ArcrWebView *view,
                                        const char  *raw_html,
                                        size_t       raw_len,
                                        const char  *ssr_snapshot,
                                        size_t       ssr_len) {
    if (!view || !view->doc || !view->doc->body || !view->engine) return false;
    if (view->engine->cfg.conformance_mode) return false;
    ArcrDocument *doc = view->doc;

    /* 1. Next.js / React 19 (`www.zuzunza.com`, `__next_f`) SSR Wipeout Recovery */
    if (ssr_snapshot && ssr_len > 0u) {
        size_t post_text = 0u, post_rich = 0u;
        hydrator_count_visible_dom(doc, doc->body, &post_text, &post_rich);
        if (post_text < 16u && post_rich == 0u) {
            arcr_dom_clear_children(doc, doc->body);
            (void)arcr_html_parse_fragment(doc, doc->body, ssr_snapshot, ssr_len);
            ArcrAtom in_atom = arcr_atom_intern_lower(&view->engine->atoms, "input", 5u);
            ArcrAtom ta_atom = arcr_atom_intern_lower(&view->engine->atoms, "textarea", 8u);
            init_form_defaults_recursive(doc, doc->body, in_atom, ta_atom);
            fetch_document_subresources(view);
            return true;
        }
    }

    char host[192];
    extract_url_host(view->url, host, sizeof(host));

    /* 2. YouTube (`youtube.com`, `youtu.be`, or embedded `ytInitialData`) */
    bool is_yt = hydrator_host_matches(host, "youtube.com") ||
                 hydrator_host_matches(host, "youtu.be") ||
                 (raw_html && raw_len > 0u &&
                  hydrator_memmem_str(raw_html, raw_len, "ytInitialData") != NULL);
    if (is_yt) {
        return hydrator_hydrate_youtube(view, raw_html, raw_len);
    }

    /* 3. X / Twitter (`x.com`, `twitter.com`) */
    if (hydrator_host_matches(host, "x.com") ||
        hydrator_host_matches(host, "twitter.com")) {
        return hydrator_hydrate_x_twitter(view);
    }

    return false;
}

#endif /* ARCR_SITE_HYDRATORS_H */
