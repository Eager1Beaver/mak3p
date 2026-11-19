#pragma once
#include "gui/presentation.hpp"
#include <FL/Fl_Group.H>
#include <FL/Fl_Scrollbar.H>

namespace audio_converter::gui {
class QueueView : public Fl_Group {
public:
    QueueView(int x, int y, int w, int h);
    std::function<void(JobId)> remove;
    std::function<void(std::string_view)> drop;
    void rows(std::vector<RowText> rows, bool editable);
    void resize(int x, int y, int w, int h) override;
    int handle(int event) override;
protected:
    void draw() override;
private:
    Fl_Scrollbar* scrollbar_;
    std::vector<RowText> rows_;
    std::string hover_;
    bool editable_ = false;
    void update_scrollbar();
};
} // namespace audio_converter::gui
