#include "editor_history.hpp"

namespace wowee {
namespace editor {

void EditorHistory::beginEdit(const pipeline::ADTTerrain& terrain,
                               const std::vector<int>& affectedChunks) {
    pending_ = {};
    pending_.before.reserve(affectedChunks.size());
    for (int idx : affectedChunks) {
        ChunkSnapshot snap;
        snap.chunkIndex = idx;
        snap.heights = terrain.chunks[idx].heightMap.heights;
        pending_.before.push_back(snap);
    }
}

void EditorHistory::endEdit(const pipeline::ADTTerrain& terrain) {
    pending_.after.reserve(pending_.before.size());
    lastAffected_.clear();
    for (const auto& snap : pending_.before) {
        ChunkSnapshot after;
        after.chunkIndex = snap.chunkIndex;
        after.heights = terrain.chunks[snap.chunkIndex].heightMap.heights;
        pending_.after.push_back(after);
        lastAffected_.push_back(snap.chunkIndex);
    }

    // Only push if something actually changed
    bool changed = false;
    for (size_t i = 0; i < pending_.before.size(); i++) {
        if (pending_.before[i].heights != pending_.after[i].heights) {
            changed = true;
            break;
        }
    }
    if (!changed) return;

    undoStack_.push_back(std::move(pending_));
    redoStack_.clear();

    if (undoStack_.size() > MAX_UNDO)
        undoStack_.erase(undoStack_.begin());
}

void EditorHistory::undo(pipeline::ADTTerrain& terrain) {
    if (undoStack_.empty()) return;

    auto cmd = std::move(undoStack_.back());
    undoStack_.pop_back();

    lastAffected_.clear();
    for (const auto& snap : cmd.before) {
        terrain.chunks[snap.chunkIndex].heightMap.heights = snap.heights;
        lastAffected_.push_back(snap.chunkIndex);
    }

    redoStack_.push_back(std::move(cmd));
}

void EditorHistory::redo(pipeline::ADTTerrain& terrain) {
    if (redoStack_.empty()) return;

    auto cmd = std::move(redoStack_.back());
    redoStack_.pop_back();

    lastAffected_.clear();
    for (const auto& snap : cmd.after) {
        terrain.chunks[snap.chunkIndex].heightMap.heights = snap.heights;
        lastAffected_.push_back(snap.chunkIndex);
    }

    undoStack_.push_back(std::move(cmd));
}

void EditorHistory::clear() {
    undoStack_.clear();
    redoStack_.clear();
    lastAffected_.clear();
}

} // namespace editor
} // namespace wowee
