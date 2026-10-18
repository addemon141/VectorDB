#pragma once

#include "Document.hh"

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace exact_index {

class Deduper {
public:
    // Returns true if kept. On keep, assigns id = documents.size()+1 and appends.
    bool try_insert(Document doc, std::vector<Document>& documents);

    size_t dropped() const { return dropped_; }

private:
    std::unordered_map<short, std::set<int>> block_index_[4];
    size_t dropped_ = 0;
};

}  // namespace exact_index
