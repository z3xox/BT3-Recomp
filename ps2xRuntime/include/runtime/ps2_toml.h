#pragma once
// Minimal TOML (subset) reader/writer for savedata/settings.toml.
//
// Hand-written on purpose: toml11 v4 needs std::source_location, which older
// clang (14) cannot compile (GCC-11's <source_location> is present but
// std::source_location is not declared under clang-14), and pulling
// a full TOML library into the clang-built runtime/launcher is not worth it for
// a fixed, flat schema. This covers exactly what settings.toml uses:
//   - '#' comments (quote-aware), [table] and [table.sub] headers
//   - scalar bool / integer / float / "quoted string"
//   - integer arrays: [1, 2, 3]

#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ps2x_toml
{
    inline std::string trim(const std::string &s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    class Document
    {
    public:
        bool parse(std::istream &in)
        {
            std::string table, line;
            while (std::getline(in, line))
            {
                std::string s = trim(stripComment(line));
                if (s.empty())
                    continue;
                if (s.front() == '[' && s.back() == ']')
                {
                    table = trim(s.substr(1, s.size() - 2));
                    continue;
                }
                const size_t eq = s.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = trim(s.substr(0, eq));
                if (key.empty())
                    continue;
                m_kv[table.empty() ? key : table + "." + key] = trim(s.substr(eq + 1));
            }
            return true;
        }

        bool has(const std::string &path) const { return m_kv.find(path) != m_kv.end(); }

        bool getB(const std::string &path, bool def) const
        {
            const std::string *v = raw(path);
            if (!v) return def;
            const std::string s = trim(*v);
            if (s == "true") return true;
            if (s == "false") return false;
            return def;
        }
        int getI(const std::string &path, int def) const
        {
            const std::string *v = raw(path);
            if (!v) return def;
            try { return static_cast<int>(std::stol(trim(*v))); } catch (...) { return def; }
        }
        double getD(const std::string &path, double def) const
        {
            const std::string *v = raw(path);
            if (!v) return def;
            try { return std::stod(trim(*v)); } catch (...) { return def; }
        }
        std::string getS(const std::string &path, const std::string &def) const
        {
            const std::string *v = raw(path);
            if (!v) return def;
            std::string s = trim(*v);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
                s = s.substr(1, s.size() - 2);
            return s;
        }
        std::vector<int> getIA(const std::string &path, const std::vector<int> &def) const
        {
            const std::string *v = raw(path);
            if (!v) return def;
            const std::string s = trim(*v);
            if (s.size() < 2 || s.front() != '[') return def;
            std::vector<int> out;
            size_t i = 1;
            while (i < s.size())
            {
                while (i < s.size() && (s[i] == ',' || std::isspace(static_cast<unsigned char>(s[i])))) ++i;
                if (i >= s.size() || s[i] == ']') break;
                size_t j = i;
                while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '-' || s[j] == '+')) ++j;
                if (j == i) { ++i; continue; }
                try { out.push_back(std::stoi(s.substr(i, j - i))); } catch (...) {}
                i = j;
            }
            return out.empty() ? def : out;
        }

    private:
        std::map<std::string, std::string> m_kv;

        const std::string *raw(const std::string &path) const
        {
            auto it = m_kv.find(path);
            return it == m_kv.end() ? nullptr : &it->second;
        }
        static std::string stripComment(const std::string &line)
        {
            bool inQuote = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"') inQuote = !inQuote;
                else if (line[i] == '#' && !inQuote) return line.substr(0, i);
            }
            return line;
        }
    };

    // --- value formatting for the writer -------------------------------------
    inline std::string fmtBool(bool v) { return v ? "true" : "false"; }
    inline std::string fmtInt(long long v) { return std::to_string(v); }
    inline std::string fmtDbl(double v)
    {
        std::ostringstream o;
        o << v;
        return o.str();
    }
    inline std::string fmtStr(const std::string &v) { return "\"" + v + "\""; }
    inline std::string fmtIntArray(const std::vector<int> &v)
    {
        std::string r = "[";
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i) r += ", ";
            r += std::to_string(v[i]);
        }
        return r + "]";
    }
} // namespace ps2x_toml
