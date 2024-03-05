#ifndef _GI_GTK_GTK_EXTRA_DEF_HPP_
#define _GI_GTK_GTK_EXTRA_DEF_HPP_

namespace gi
{
namespace repository
{
namespace Gtk
{
namespace base
{
template<typename... Args>
Gtk::ListStore
ListStoreExtra::new_type_() noexcept
{
  GType columns[] = {traits::gtype<Args>::get_type()...};
  return ListStoreBase::new_(G_N_ELEMENTS(columns), columns);
};
} // namespace base

} // namespace Gtk

} // namespace repository

} // namespace gi

#endif
