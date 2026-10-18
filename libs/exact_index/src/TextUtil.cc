#include "TextUtil.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <tinyxml2.h>
#include <utfcpp/utf8.h>

namespace exact_index {

using namespace tinyxml2;

namespace {

#define IS_CJK_CHAR(c)                                                         \
    (((c) >= 0x4E00 && (c) <= 0x9FFF) || ((c) >= 0x3400 && (c) <= 0x4DBF) ||    \
     ((c) >= 0x20000 && (c) <= 0x2A6DF) || ((c) >= 0x2A700 && (c) <= 0x2B73F) || \
     ((c) >= 0x2B740 && (c) <= 0x2B81F) || ((c) >= 0x2B820 && (c) <= 0x2CEAF) || \
     ((c) >= 0x2CEB0 && (c) <= 0x2EBEF) || ((c) >= 0x30000 && (c) <= 0x3134F) || \
     ((c) >= 0xF900 && (c) <= 0xFAFF) || ((c) >= 0x2F800 && (c) <= 0x2FA1F))

std::string element_text(XMLElement* el) {
    if (!el) {
        return {};
    }
    const char* t = el->GetText();
    return t ? std::string(t) : std::string();
}

}  // namespace

bool Stopwords::load(const std::string& cn_path, const std::string& en_path) {
    auto load_set = [](const std::string& path, std::unordered_set<std::string>& out) {
        std::ifstream ifs(path);
        if (!ifs) {
            return false;
        }
        std::string line;
        while (std::getline(ifs, line)) {
            std::istringstream iss(line);
            std::string word;
            if (iss >> word) {
                out.insert(word);
            }
        }
        return true;
    };
    return load_set(cn_path, cn_) && load_set(en_path, en_);
}

bool Stopwords::is_cn_stop(const std::string& word) const {
    return cn_.find(word) != cn_.end();
}

bool Stopwords::is_en_stop(const std::string& word) const {
    return en_.find(word) != en_.end();
}

std::string strip_html(const std::string& text) {
    static const std::regex pattern(R"(<[^>]*>)");
    return std::regex_replace(text, pattern, "");
}

bool valid_cn_word(const std::string& word, const Stopwords& sw) {
    auto it = utf8::iterator<std::string::const_iterator>(word.begin(), word.begin(),
                                                          word.end());
    auto end =
        utf8::iterator<std::string::const_iterator>(word.end(), word.begin(), word.end());
    if (it == end) {
        return false;
    }
    while (it != end) {
        if (!IS_CJK_CHAR(*it)) {
            return false;
        }
        ++it;
    }
    return !sw.is_cn_stop(word);
}

bool valid_en_word(const std::string& word, const Stopwords& sw) {
    if (word.empty()) {
        return false;
    }
    for (unsigned char ch : word) {
        if (!std::isalpha(ch)) {
            return false;
        }
    }
    return !sw.is_en_stop(word);
}

std::vector<std::string> list_xml_files(const std::string& dir) {
    std::vector<std::string> files;
    DIR* dirp = opendir(dir.c_str());
    if (!dirp) {
        return files;
    }
    std::string base = dir;
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    while (dirent* ent = readdir(dirp)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (name.size() >= 4 && name.substr(name.size() - 4) == ".xml") {
            files.push_back(base + name);
        }
    }
    closedir(dirp);
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<Document> parse_rss_xml(const std::string& path) {
    std::vector<Document> docs;
    XMLDocument xml;
    if (xml.LoadFile(path.c_str()) != XML_SUCCESS) {
        return docs;
    }
    XMLElement* rss = xml.RootElement();
    if (!rss) {
        return docs;
    }
    XMLElement* channel = rss->FirstChildElement("channel");
    if (!channel) {
        return docs;
    }
    for (XMLElement* item = channel->FirstChildElement("item"); item;
         item = item->NextSiblingElement("item")) {
        XMLElement* content = item->FirstChildElement("content");
        std::string s_content;
        if (content) {
            s_content = element_text(content);
        } else {
            XMLElement* description = item->FirstChildElement("description");
            if (!description) {
                continue;
            }
            s_content = element_text(description);
        }
        s_content = strip_html(s_content);
        Document doc;
        doc.content = std::move(s_content);
        doc.link = element_text(item->FirstChildElement("link"));
        doc.title = element_text(item->FirstChildElement("title"));
        docs.push_back(std::move(doc));
    }
    return docs;
}

double generate_idf(double n, double df) {
    return std::log2(n / (df + 1.0));
}

void l2_normalize(std::map<std::string, double>& values) {
    double sum = 0.0;
    for (auto& [_, v] : values) {
        sum += v * v;
    }
    if (sum <= 0.0) {
        return;
    }
    double denom = std::sqrt(sum);
    for (auto& [_, v] : values) {
        v /= denom;
    }
}

}  // namespace exact_index
