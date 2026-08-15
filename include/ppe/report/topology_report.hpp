// topology_report.hpp -- render a platform_topology as ASCII art and as HTML (#6).
//
// Both renderers collapse consecutive clusters of identical shape into one line
// with a multiplier. Eight separate entries for eight identical P-cores is
// accurate and unreadable; "8 x [1 core ...]" is both. The collapsing happens
// here, at render time -- the data keeps every cluster, because a consumer that
// wants to reason about cluster 5 specifically should be able to.
//
// ASCII only, per the repository's source-encoding rule, so the tree is drawn
// with +-- and | rather than box-drawing characters. That also makes it safe to
// paste into a terminal, an issue, or a commit message, which is most of what a
// topology dump is for.
#pragma once

#include <ppe/detect/topology.hpp>
#include <ppe/provenance.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace ppe::report {

namespace detail {

inline std::string bytes_human(std::size_t b) {
    char buf[32];
    if (b == 0) return "n/a";
    if (b >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.4g MiB", static_cast<double>(b) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.4g KiB", static_cast<double>(b) / 1024.0);
    }
    return std::string(buf);
}

/// How a cache instance is shared, in words rather than a bare number.
inline std::string sharing_note(std::size_t sharers) {
    if (sharers == 0) return "sharing unknown";
    if (sharers == 1) return "private";
    return "shared by " + std::to_string(sharers) + " cores";
}

struct run {
    const core_cluster* cluster;
    unsigned count;
};

/// Collapse consecutive identical clusters.
inline std::vector<run> collapse(const std::vector<core_cluster>& clusters) {
    std::vector<run> runs;
    for (const core_cluster& c : clusters) {
        if (!runs.empty() && runs.back().cluster->same_shape_as(c)) {
            ++runs.back().count;
        } else {
            runs.push_back(run{&c, 1});
        }
    }
    return runs;
}

inline std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace detail

/// ASCII tree of the machine.
inline std::string to_ascii(const platform_topology& t) {
    std::string s;
    char buf[512];

    std::snprintf(buf, sizeof(buf), "%s\n", t.name.c_str());
    s += buf;
    std::snprintf(buf, sizeof(buf),
                  "  %u package(s), %u physical cores, %u logical processors,"
                  " %u NUMA domain(s)\n",
                  t.packages, t.physical_cores, t.logical_processors, t.numa_domains);
    s += buf;
    std::snprintf(buf, sizeof(buf), "  cache line %s, topology via %s\n\n",
                  t.cache_line_bytes ? (std::to_string(t.cache_line_bytes) + " bytes").c_str()
                                     : "unknown",
                  t.source.empty() ? "no backend" : t.source.c_str());
    s += buf;

    if (t.clusters.empty()) {
        s += "  no cluster topology detected\n";
        return s;
    }

    const std::vector<detail::run> runs = detail::collapse(t.clusters);

    for (std::size_t i = 0; i < runs.size(); ++i) {
        const core_cluster& c = *runs[i].cluster;
        const bool last = (i + 1 == runs.size()) && t.l3_bytes == 0;
        const char* stem = last ? "\\--" : "+--";
        const char* pipe = last ? "   " : "|  ";

        std::string title = "cluster";
        if (runs[i].count > 1) title = std::to_string(runs[i].count) + " x cluster";
        std::snprintf(buf, sizeof(buf), "%s %s", stem, title.c_str());
        s += buf;
        if (!c.role.empty()) {
            std::snprintf(buf, sizeof(buf), "  [%s]", c.role.c_str());
            s += buf;
        }
        s += "\n";

        std::snprintf(buf, sizeof(buf), "%s    %u core(s), %u thread(s)\n", pipe,
                      c.physical_cores, c.logical_processors);
        s += buf;
        std::snprintf(buf, sizeof(buf), "%s    L1d %-10s (%s)\n", pipe,
                      detail::bytes_human(c.l1d_bytes).c_str(),
                      detail::sharing_note(c.l1d_sharing_cores).c_str());
        s += buf;
        if (c.l1i_bytes != 0) {
            std::snprintf(buf, sizeof(buf), "%s    L1i %-10s\n", pipe,
                          detail::bytes_human(c.l1i_bytes).c_str());
            s += buf;
        }
        std::snprintf(buf, sizeof(buf), "%s    L2  %-10s (%s)\n", pipe,
                      detail::bytes_human(c.l2_bytes).c_str(),
                      detail::sharing_note(c.l2_sharing_cores).c_str());
        s += buf;
        if (c.capacity != 0) {
            std::snprintf(buf, sizeof(buf), "%s    capacity %zu (%s)\n", pipe, c.capacity,
                          t.capacity_source.empty() ? "unknown source"
                                                    : t.capacity_source.c_str());
            s += buf;
        }
        if (!last) s += "|\n";
    }

    if (t.l3_bytes != 0) {
        std::snprintf(buf, sizeof(buf), "\\-- L3  %s (%s)\n",
                      detail::bytes_human(t.l3_bytes).c_str(),
                      detail::sharing_note(t.l3_sharing_cores).c_str());
        s += buf;
    }

    if (t.heterogeneous()) {
        s += "\nHETEROGENEOUS: clusters differ. A measurement that does not say which\n";
        s += "cluster it ran on describes neither of them -- pin with taskset.\n";
    }
    return s;
}

/// Self-contained HTML. No external requests: a report that needs a network to
/// render is not a report you can keep.
inline std::string to_html(const platform_topology& t, const provenance& prov) {
    using detail::html_escape;
    std::string h;
    h += "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    h += "<title>PPE topology -- " + html_escape(t.name) + "</title>\n";
    h += "<style>\n"
         ":root{--bg:#fbfbfa;--fg:#1a1a19;--mut:#6b6b68;--line:#dcdcd8;"
         "--card:#fff;--accent:#1f7a58;--warn:#8a5a00}\n"
         "@media(prefers-color-scheme:dark){:root{--bg:#16171a;--fg:#e8e8e6;"
         "--mut:#9a9a97;--line:#2e3034;--card:#1d1f23;--accent:#2fa37a;--warn:#d1a04a}}\n"
         "*{box-sizing:border-box}body{margin:0;padding:2rem 1.25rem;background:var(--bg);"
         "color:var(--fg);font:15px/1.55 ui-sans-serif,system-ui,-apple-system,"
         "'Segoe UI',Roboto,sans-serif}\n"
         ".wrap{max-width:60rem;margin:0 auto}\n"
         "h1{font-size:1.5rem;margin:0 0 .25rem}\n"
         ".sub{color:var(--mut);margin:0 0 1.5rem;font-size:.9rem}\n"
         ".facts{display:flex;flex-wrap:wrap;gap:.5rem 1.5rem;margin:0 0 1.5rem;"
         "padding:.75rem 1rem;border:1px solid var(--line);border-radius:8px;"
         "background:var(--card)}\n"
         ".fact b{display:block;font-size:.72rem;text-transform:uppercase;"
         "letter-spacing:.05em;color:var(--mut);font-weight:600}\n"
         ".pkg{border:1px solid var(--line);border-radius:10px;padding:1rem;"
         "background:var(--card)}\n"
         ".clusters{display:flex;flex-wrap:wrap;gap:.75rem;margin:.75rem 0}\n"
         ".cl{flex:1 1 15rem;border:1px solid var(--line);border-radius:8px;"
         "padding:.75rem .9rem;background:var(--bg)}\n"
         ".cl h3{margin:0 0 .5rem;font-size:.95rem;display:flex;"
         "justify-content:space-between;align-items:baseline;gap:.5rem}\n"
         ".role{font-size:.7rem;font-weight:600;text-transform:uppercase;"
         "letter-spacing:.05em;color:var(--accent)}\n"
         ".cores{display:flex;flex-wrap:wrap;gap:3px;margin:.4rem 0 .6rem}\n"
         ".core{width:1.35rem;height:1.35rem;border-radius:4px;"
         "background:var(--accent);opacity:.85}\n"
         "table{width:100%;border-collapse:collapse;font-size:.85rem}\n"
         "td{padding:.15rem 0;vertical-align:top}\n"
         "td.k{color:var(--mut);padding-right:.75rem;white-space:nowrap}\n"
         ".l3{margin-top:.75rem;padding:.6rem .9rem;border:1px dashed var(--line);"
         "border-radius:8px;font-size:.88rem}\n"
         ".note{margin-top:1.25rem;padding:.75rem 1rem;border-left:3px solid var(--warn);"
         "background:var(--card);font-size:.88rem}\n"
         "footer{margin-top:2rem;color:var(--mut);font-size:.78rem;"
         "border-top:1px solid var(--line);padding-top:.75rem}\n"
         "code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}\n"
         "</style>\n</head>\n<body>\n<div class=\"wrap\">\n";

    h += "<h1>" + html_escape(t.name) + "</h1>\n";
    h += "<p class=\"sub\">Platform topology, detected via <code>" +
         html_escape(t.source.empty() ? "no backend" : t.source) + "</code></p>\n";

    h += "<div class=\"facts\">\n";
    auto fact = [&](const char* k, const std::string& v) {
        h += "<div class=\"fact\"><b>" + std::string(k) + "</b>" + html_escape(v) +
             "</div>\n";
    };
    fact("packages", std::to_string(t.packages));
    fact("physical cores", std::to_string(t.physical_cores));
    fact("logical processors", std::to_string(t.logical_processors));
    fact("NUMA domains", std::to_string(t.numa_domains));
    fact("cache line",
         t.cache_line_bytes ? std::to_string(t.cache_line_bytes) + " bytes" : "unknown");
    fact("clusters", std::to_string(t.clusters.size()));
    h += "</div>\n";

    h += "<div class=\"pkg\">\n<div class=\"clusters\">\n";
    for (const detail::run& r : detail::collapse(t.clusters)) {
        const core_cluster& c = *r.cluster;
        h += "<div class=\"cl\">\n<h3><span>";
        h += (r.count > 1 ? std::to_string(r.count) + " x cluster" : std::string("cluster"));
        h += "</span>";
        if (!c.role.empty()) h += "<span class=\"role\">" + html_escape(c.role) + "</span>";
        h += "</h3>\n";

        // One box per core, repeated per identical cluster: the shape of the
        // machine should be visible before any number is read.
        h += "<div class=\"cores\">";
        for (unsigned k = 0; k < r.count * c.physical_cores && k < 256; ++k) {
            h += "<div class=\"core\"></div>";
        }
        h += "</div>\n";

        h += "<table>\n";
        auto row = [&](const char* k, const std::string& v) {
            h += "<tr><td class=\"k\">" + std::string(k) + "</td><td>" + html_escape(v) +
                 "</td></tr>\n";
        };
        row("cores", std::to_string(c.physical_cores) + " (" +
                         std::to_string(c.logical_processors) + " threads)");
        row("L1d", detail::bytes_human(c.l1d_bytes) + " (" +
                       detail::sharing_note(c.l1d_sharing_cores) + ")");
        if (c.l1i_bytes) row("L1i", detail::bytes_human(c.l1i_bytes));
        row("L2", detail::bytes_human(c.l2_bytes) + " (" +
                      detail::sharing_note(c.l2_sharing_cores) + ")");
        if (c.capacity) {
            row("capacity", std::to_string(c.capacity) + " (" +
                                (t.capacity_source.empty() ? "unknown source"
                                                           : t.capacity_source) + ")");
        }
        h += "</table>\n</div>\n";
    }
    h += "</div>\n";

    if (t.l3_bytes) {
        h += "<div class=\"l3\">L3 " + html_escape(detail::bytes_human(t.l3_bytes)) +
             " (" + html_escape(detail::sharing_note(t.l3_sharing_cores)) + ")</div>\n";
    }
    h += "</div>\n";

    if (t.heterogeneous()) {
        h += "<div class=\"note\"><b>Heterogeneous.</b> The clusters above differ. A "
             "measurement that does not record which cluster it ran on describes "
             "neither of them &mdash; pin the process before measuring.</div>\n";
    }

    h += "<footer>PPE " + html_escape(prov.ppe_version) + " &middot; commit " +
         html_escape(prov.git_commit);
    if (prov.git_dirty == "1") h += " (dirty)";
    h += " &middot; " + html_escape(prov.compiler) + " &middot; build ISA " +
         html_escape(prov.isa) + " &middot; " + html_escape(prov.utc_timestamp) +
         "</footer>\n";
    h += "</div>\n</body>\n</html>\n";
    return h;
}

/// The topology as JSON, so the HTML page and any future visualization render
/// from one machine-readable form rather than two hand-maintained ones.
inline std::string to_json(const platform_topology& t) {
    std::string j = "{\n";
    j += "  \"name\": \"" + detail::html_escape(t.name) + "\",\n";
    j += "  \"source\": \"" + t.source + "\",\n";
    j += "  \"capacity_source\": \"" + t.capacity_source + "\",\n";
    j += "  \"packages\": " + std::to_string(t.packages) + ",\n";
    j += "  \"physical_cores\": " + std::to_string(t.physical_cores) + ",\n";
    j += "  \"logical_processors\": " + std::to_string(t.logical_processors) + ",\n";
    j += "  \"numa_domains\": " + std::to_string(t.numa_domains) + ",\n";
    j += "  \"cache_line_bytes\": " + std::to_string(t.cache_line_bytes) + ",\n";
    j += "  \"l3_bytes\": " + std::to_string(t.l3_bytes) + ",\n";
    j += "  \"l3_sharing_cores\": " + std::to_string(t.l3_sharing_cores) + ",\n";
    j += "  \"heterogeneous\": " + std::string(t.heterogeneous() ? "true" : "false") + ",\n";
    j += "  \"clusters\": [\n";
    for (std::size_t i = 0; i < t.clusters.size(); ++i) {
        const core_cluster& c = t.clusters[i];
        j += "    {\"physical_cores\": " + std::to_string(c.physical_cores);
        j += ", \"logical_processors\": " + std::to_string(c.logical_processors);
        j += ", \"package\": " + std::to_string(c.package);
        j += ", \"l1d_bytes\": " + std::to_string(c.l1d_bytes);
        j += ", \"l1i_bytes\": " + std::to_string(c.l1i_bytes);
        j += ", \"l2_bytes\": " + std::to_string(c.l2_bytes);
        j += ", \"l1d_sharing_cores\": " + std::to_string(c.l1d_sharing_cores);
        j += ", \"l2_sharing_cores\": " + std::to_string(c.l2_sharing_cores);
        j += ", \"capacity\": " + std::to_string(c.capacity);
        j += ", \"role\": \"" + c.role + "\"}";
        j += (i + 1 == t.clusters.size()) ? "\n" : ",\n";
    }
    j += "  ]\n}\n";
    return j;
}

}  // namespace ppe::report
