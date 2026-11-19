#include "gui/queue_view.hpp"
#include "gui/constants.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>
#include <algorithm>

namespace audio_converter::gui {
namespace {
std::string shortened(std::string value, int width) {
    if (fl_width(value.c_str()) <= width) return value;
    while (!value.empty() && fl_width((value + "…").c_str()) > width) {
        do { value.pop_back(); } while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0) == 0x80);
        // Remove the leading byte of a partially removed multibyte character.
        if (!value.empty() && static_cast<unsigned char>(value.back()) >= 0xc0) value.pop_back();
    }
    return value + "…";
}
}
QueueView::QueueView(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    scrollbar_ = new Fl_Scrollbar(x + w - metrics::scrollbar_width, y, metrics::scrollbar_width, h);
    scrollbar_->type(FL_VERTICAL); scrollbar_->step(metrics::row_height / 2);
    scrollbar_->callback([](Fl_Widget*, void* data) { static_cast<QueueView*>(data)->redraw(); }, this);
    end();
}
void QueueView::update_scrollbar() {
    const int total = static_cast<int>(rows_.size()) * metrics::row_height;
    scrollbar_->value(std::clamp(scrollbar_->value(), 0, std::max(0, total - h())), h(), 0, std::max(h(), total));
    if (total > h()) scrollbar_->show(); else scrollbar_->hide();
}
void QueueView::rows(std::vector<RowText> rows, bool editable) {
    bool changed = editable != editable_ || rows.size() != rows_.size();
    if (!changed) for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& a = rows[i]; const auto& b = rows_[i];
        if (a.id != b.id || a.name != b.name || a.properties != b.properties || a.status != b.status ||
            a.remaining != b.remaining || a.progress != b.progress || a.warning != b.warning || a.failed != b.failed ||
            a.progress_visible != b.progress_visible) { changed = true; break; }
    }
    rows_ = std::move(rows); editable_ = editable; update_scrollbar(); if (changed) redraw();
}

} // namespace audio_converter::gui
