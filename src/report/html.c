// Standalone web view of the whole-ROM memory map: one small square per
// 64-byte block (coloured code / data / padding / unknown), the summary and
// the discovery log. No external dependency, light and dark friendly.
#include "report.h"
#include <stdlib.h>
#include <string.h>

static void html_escape(DpBuf *out, const char *s) {
    for (; *s; s++) {
        if (*s == '<') dp_buf_str(out, "&lt;");
        else if (*s == '>') dp_buf_str(out, "&gt;");
        else if (*s == '&') dp_buf_str(out, "&amp;");
        else if (*s == '"') dp_buf_str(out, "&quot;");
        else dp_buf_fmt(out, "%c", *s);
    }
}

// Block letter: C code, D data, P padding, ? unknown, M mixed code+data.
static char block_letter(const uint8_t *cls, size_t n) {
    unsigned code = 0, data = 0, pad = 0, unk = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = cls[i];
        if (c == DP_MAP_UNKNOWN) unk++;
        else if (dp_map_is_code(c)) code++;
        else if (c == DP_MAP_PAD || c == DP_MAP_INF_PAD) pad++;
        else data++;
    }
    if (unk * 2 > n) return '?';
    if (code && data && code * 8 > n && data * 8 > n) return 'M';
    if (code >= data && code >= pad) return code ? 'C' : '?';
    if (data >= pad) return 'D';
    return 'P';
}

void dp_report_html(const DpResults *res, DpBuf *out) {
    DpMapSummary sum;
    uint8_t *cls = dp_map_build(res, &sum);
    if (!cls) return;
    double t = sum.total ? 100.0 / (double)sum.total : 0.0;
    dp_buf_str(out, "<!doctype html>\n<html lang=\"fr\"><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>deepeel map</title>\n<style>\n"
        ":root{--bg:#fafafa;--fg:#1b1b1f;--mut:#666;--card:#fff;--bd:#d8d8de;--code:#2f7fd8;--data:#2fa56a;"
        "--pad:#a8a8b3;--unk:#d8453b;--mix:#8a5bd0}\n"
        "@media(prefers-color-scheme:dark){:root{--bg:#141418;--fg:#e8e8ee;--mut:#9a9aa6;--card:#1e1e24;--bd:#34343c;"
        "--code:#4a9bf0;--data:#43c283;--pad:#6c6c78;--unk:#f06a5f;--mix:#a883e6}}\n"
        "body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}\n"
        "h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:24px 0 8px}\n"
        ".mut{color:var(--mut)}.card{background:var(--card);border:1px solid var(--bd);border-radius:8px;padding:12px;"
        "margin:8px 0;overflow-x:auto}\n"
        ".grid{display:grid;grid-template-columns:repeat(32,1fr);gap:2px;max-width:640px}\n"
        ".grid i{aspect-ratio:1;border-radius:2px;display:block}\n"
        ".C{background:var(--code)}.D{background:var(--data)}.P{background:var(--pad)}.U{background:var(--unk)}.M{background:var(--mix)}\n"
        ".lg span{display:inline-block;margin-right:14px}.lg i{display:inline-block;width:12px;height:12px;border-radius:2px;"
        "vertical-align:-1px;margin-right:4px}\n"
        "table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:3px 8px;border-bottom:1px solid var(--bd);"
        "font-variant-numeric:tabular-nums}th{color:var(--mut);font-weight:600}\n"
        "td.n,th.n{text-align:right}\n</style></head><body>\n");
    dp_buf_str(out, "<h1>deepeel : memory map de la ROM</h1>\n<div class=\"mut\">");
    html_escape(out, res->cart->title);
    dp_buf_fmt(out, " &middot; %d banques de 16 Ko &middot; %llu octets &middot; observations de l'outil "
               "(machine concr&egrave;te, RAM &agrave; z&eacute;ro), pas des mesures mat&eacute;rielles</div>\n",
               res->banks, (unsigned long long)sum.total);
    dp_buf_fmt(out, "<div class=\"card\"><b>%.1f%% connu</b> &middot; %.1f%% inconnu &middot; %.1f%% code &middot; "
               "%.1f%% data &middot; %.1f%% padding <span class=\"mut\">(le connu inclut %.1f%% d'inf&eacute;r&eacute;)</span></div>\n",
               sum.known * t, sum.unknown * t, sum.code * t, sum.data * t, sum.pad * t, sum.inferred * t);
    dp_buf_str(out, "<div class=\"lg\"><span><i class=\"C\"></i>code</span><span><i class=\"D\"></i>data</span>"
                    "<span><i class=\"P\"></i>padding</span><span><i class=\"M\"></i>code+data</span>"
                    "<span><i class=\"U\"></i>inconnu</span></div>\n");
    dp_buf_str(out, "<h2>Carr&eacute;s de 64 octets par banque</h2>\n<div id=\"banks\"></div>\n<script>\nconst B=[\n");
    for (int b = 0; b < res->banks; b++) {
        dp_buf_str(out, "\"");
        for (size_t o = 0; o < 0x4000; o += 64) dp_buf_fmt(out, "%c", block_letter(cls + (size_t)b * 0x4000 + o, 64));
        dp_buf_str(out, b + 1 < res->banks ? "\",\n" : "\"\n");
    }
    dp_buf_str(out, "];\nconst N={C:'code',D:'data',P:'padding',M:'code+data','?':'inconnu'};\n"
        "const root=document.getElementById('banks');\n"
        "B.forEach((s,b)=>{const d=document.createElement('div');d.className='card';"
        "d.innerHTML='<b>Banque '+b+'</b> <span class=\"mut\">'+(b?'4000-7fff':'0000-3fff')+'</span>';"
        "const g=document.createElement('div');g.className='grid';"
        "[...s].forEach((c,i)=>{const e=document.createElement('i');e.className=c=='?'?'U':c;"
        "const a=(b?0x4000:0)+i*64;e.title='banque '+b+' '+a.toString(16).padStart(4,'0')+' : '+N[c];g.appendChild(e)});"
        "d.appendChild(g);root.appendChild(d)});\n</script>\n");
    dp_buf_str(out, "<h2>R&eacute;partition par classe</h2>\n<div class=\"card\"><table><tr><th>classe</th>"
                    "<th class=\"n\">octets</th><th class=\"n\">%</th></tr>\n");
    for (int c = 0; c < DP_MAP_COUNT; c++)
        dp_buf_fmt(out, "<tr><td>%s</td><td class=\"n\">%llu</td><td class=\"n\">%.1f</td></tr>\n", dp_map_class_name(c),
                   (unsigned long long)sum.cls[c], sum.cls[c] * t);
    dp_buf_str(out, "</table></div>\n<h2>Journal de d&eacute;couverte</h2>\n<div class=\"card\"><table><tr>"
                    "<th class=\"n\">temps (s)</th><th>source</th><th class=\"n\">cr&eacute;dit</th>"
                    "<th class=\"n\">octets connus</th><th class=\"n\">%</th></tr>\n");
    for (int i = 0; i < res->disc_n; i++) {
        const DpDiscEntry *e = &res->disc[i];
        dp_buf_fmt(out, "<tr><td class=\"n\">%.2f</td><td>", e->t);
        html_escape(out, e->source);
        dp_buf_fmt(out, "</td><td class=\"n\">%llu</td><td class=\"n\">%u</td><td class=\"n\">%.1f</td></tr>\n",
                   (unsigned long long)e->credit, e->known, e->known * t);
    }
    dp_buf_str(out, "</table></div>\n</body></html>\n");
    free(cls);
}
