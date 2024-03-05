//#define GI_INLINE 1
#include <gtk/gtk.hpp>

#include <iostream>
#include <tuple>
#include <vector>

namespace GLib = gi::repository::GLib;
namespace GObject_ = gi::repository::GObject;
namespace Gtk = gi::repository::Gtk;

static GLib::MainLoop loop;

// based on python-gtk3 example
// https://python-gtk-3-tutorial.readthedocs.io/en/latest/treeview.html

// list of tuples for each software,
// containing the software name, initial release, and main programming languages
const std::vector<std::tuple<std::string, int, std::string>> software_list{
    std::make_tuple("Firefox", 2002, "C++"),
    std::make_tuple("Eclipse", 2004, "Java"),
    std::make_tuple("Pitivi", 2004, "Python"),
    std::make_tuple("Netbeans", 1996, "Java"),
    std::make_tuple("Chrome", 2008, "C++"),
    std::make_tuple("Filezilla", 2001, "C++"),
    std::make_tuple("Bazaar", 2005, "Python"),
    std::make_tuple("Git", 2005, "C"),
    std::make_tuple("Linux Kernel", 1991, "C"),
    std::make_tuple("GCC", 1987, "C"),
    std::make_tuple("Frostwire", 2004, "Java")};

class TreeViewFilterWindow : public Gtk::impl::WindowImpl
{
  typedef TreeViewFilterWindow self_type;

  Gtk::ListStore store_;
  Gtk::TreeModelFilter language_filter_;
  std::string current_filter_language_;

public:
  TreeViewFilterWindow() : Gtk::impl::WindowImpl(this)
  {
    Gtk::Window &self = *(this);
    self.set_title("TreeView filter demo");
    self.set_border_width(10);

    // set up the grid in which elements are positioned
    auto grid = Gtk::Grid::new_();
    grid.set_column_homogeneous(true);
    grid.set_row_homogeneous(true);
    self.add(grid);

    // create ListStore model
    store_ = Gtk::ListStore::new_type_<std::string, int, std::string>();
    for (auto &e : software_list) {
      auto it = store_.append();
      GObject_::Value cols[] = {std::get<0>(e), std::get<1>(e), std::get<2>(e)};
      for (unsigned i = 0; i < G_N_ELEMENTS(cols); ++i) {
        store_.set_value(it, i, cols[i]);
      }
    }

    // create the filter, feeding it with the liststore model
    auto treemodel = store_.interface_(gi::interface_tag<Gtk::TreeModel>());
    language_filter_ =
        gi::object_cast<Gtk::TreeModelFilter>(treemodel.filter_new(nullptr));
    // set the filter function
    language_filter_.set_visible_func(
        gi::mem_fun(&self_type::language_filter_func, this));

    // create the treeview, make it use the filter as a model, and add
    // columns
    auto treeview = Gtk::TreeView::new_with_model(language_filter_);
    int i = 0;
    for (auto &e : {"Software", "Release Year", "Programming Language"}) {
      auto renderer = Gtk::CellRendererText::new_();
      auto column = Gtk::TreeViewColumn::new_(e, renderer, {{"text", i}});
      treeview.append_column(column);
      ++i;
    }

    // create buttons to filter by programming language, and set up their
    // events
    std::vector<Gtk::Widget> buttons;
    for (auto &prog_language : {"Java", "C", "C++", "Python", "None"}) {
      auto button = Gtk::Button::new_with_label(prog_language);
      buttons.push_back(button);
      button.signal_clicked().connect(
          gi::mem_fun(&self_type::on_selection_button_clicked, this));
    }

    // set up the layout;
    // put the treeview in a scrollwindow, and the buttons in a row
    auto scrollable_treelist = Gtk::ScrolledWindow::new_();
    scrollable_treelist.set_vexpand(true);
    grid.attach(scrollable_treelist, 0, 0, 8, 10);
    grid.attach_next_to(
        buttons[0], scrollable_treelist, Gtk::PositionType::BOTTOM_, 1, 1);
    auto it = buttons.begin() + 1;
    while (it != buttons.end()) {
      grid.attach_next_to(*it, *(it - 1), Gtk::PositionType::RIGHT_, 1, 1);
      ++it;
    }
    scrollable_treelist.add(treeview);

    self.show_all();
  }

  bool language_filter_func(Gtk::TreeModel filter, Gtk::TreeIter_Ref it) const
  {
    if (current_filter_language_.empty() || current_filter_language_ == "None")
      return true;
    return current_filter_language_ ==
           filter.get_value(it, 2).get_value<std::string>();
  }

  void on_selection_button_clicked(Gtk::Button button)
  {
    // set the current language filter to the button's label
    current_filter_language_ = button.get_label();
    std::cout << current_filter_language_ << " language selected!" << std::endl;
    //  update the filter, which updates in turn the view
    language_filter_.refilter();
  }
};

int
main(int argc, char **argv)
{
  gtk_init(&argc, &argv);

  // recommended general approach iso stack based
  // too much vmethod calling which is not safe for plain case
  auto win = gi::make_ref<TreeViewFilterWindow>();
  // TODO auto-handle arg ignore ??
  win->signal_destroy().connect([](Gtk::Widget) { Gtk::main_quit(); });
  win->show_all();
  Gtk::main();
}
