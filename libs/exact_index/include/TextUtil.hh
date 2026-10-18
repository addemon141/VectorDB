#pragma once

#include "Document.hh"

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace exact_index {

class Stopwords {
public:
    bool load(const std::string& cn_path, const std::string& en_path);

    bool is_cn_stop(const std::string& word) const;
    bool is_en_stop(const std::string& word) const;

private:
    std::unordered_set<std::string> cn_;
    std::unordered_set<std::string> en_;
};

std::string strip_html(const std::string& text);
bool valid_cn_word(const std::string& word, const Stopwords& sw);
bool valid_en_word(const std::string& word, const Stopwords& sw);

std::vector<std::string> list_xml_files(const std::string& dir);
std::vector<Document> parse_rss_xml(const std::string& path);

double generate_idf(double n, double df);
void l2_normalize(std::map<std::string, double>& values);

}  // namespace exact_index
