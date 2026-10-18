#include "Deduper.hh"

#include <simhash/Simhasher.hpp>

namespace exact_index {

bool Deduper::try_insert(Document doc, std::vector<Document>& documents) {
    uint64_t h = doc.simhash;
    short* blocks = reinterpret_cast<short*>(&h);
    std::map<int, int> hitted_docid;

    for (int i = 0; i < 4; ++i) {
        auto it = block_index_[i].find(blocks[i]);
        if (it == block_index_[i].end()) {
            continue;
        }
        for (int id : it->second) {
            hitted_docid[id] += 1;
        }
    }

    for (auto& [id, times] : hitted_docid) {
        if (times == 4) {
            ++dropped_;
            return false;
        }
        if (times > 1) {
            if (simhash::Simhasher::isEqual(documents[id - 1].simhash, doc.simhash, 3)) {
                ++dropped_;
                return false;
            }
        }
    }

    doc.id = static_cast<int>(documents.size()) + 1;
    documents.push_back(std::move(doc));
    const Document& kept = documents.back();
    uint64_t kh = kept.simhash;
    short* kb = reinterpret_cast<short*>(&kh);
    for (int i = 0; i < 4; ++i) {
        block_index_[i][kb[i]].insert(kept.id);
    }
    return true;
}

}  // namespace exact_index
