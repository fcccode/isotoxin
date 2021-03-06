#include "rectangles.h"

void TSCALL menu_handler_stub(const ts::str_c &)
{
    WARNING("action not handled");
}


menu_c& menu_c::add( const ts::wstr_c & text, ts::uint32 flags, MENUHANDLER h, const ts::asptr& param, ts::bitmap_c *icon )
{
    flags = flags & 0x0000ffff;

    if (curbp == nullptr) prepare();
    ts::wbp_c &item = curbp->add_block(text);

    if ( icon )
    {
        uint index = (uint)core->register_bitmap( *icon );
        flags |= index << 16;
    }

    item.set_value( to_wstr(encode(flags,h,param)) );
    return *this;
}

menu_c& menu_c::add_separator()
{
    if (curbp == nullptr) return *this; // no need separator as 1st menu element
    ts::wbp_c &item = curbp->add_block(ts::wstr_c());
    item.set_value( CONSTWSTR("\1s") );
    return *this;
}

menu_c menu_c::add_path( const ts::wstr_c & path )
{
    if (curbp == nullptr) prepare();
    menu_c m = *this;
    for( ts::token<ts::wchar> t( path, '/' ); t; ++t )
    {
        if (m.curbp == nullptr) m.prepare();
        ts::wbp_c *item = m.curbp->get(*t);
        if (!item) item = &m.curbp->add_block(*t);
        item->set_value(CONSTWSTR("\1t"));
        m = menu_c(m, item);
    }
    return m;
}

menu_c menu_c::add_sub( const ts::wstr_c & text )
{
    if (curbp == nullptr) prepare();
    ts::wbp_c &item = curbp->add_block(text);
    item.set_value( CONSTWSTR("\1t") );
    return menu_c( *this, &item );
}

menu_c menu_c::get_sub( const ts::wbp_c &bp ) const
{
    if (ASSERT(core && core->bp.present_r(&bp)))
    {
        return menu_c( *this, const_cast<ts::wbp_c *>(&bp) );
    }
    return menu_c();
}

ts::wstr_c menu_c::get_text(int index) const
{
    struct getta
    {
        int index;
        ts::wstr_c t;

        getta(int index) :index(index) {}
        bool operator()(getta&, const ts::wsptr&) {return true;} // skip separator
        bool operator()(getta&, const ts::wsptr&, const menu_c&) {return true;} // skip submenu
        bool operator()(getta&, const menu_item_info_s &inf)
        {
            if (index == 0)
            {
                t = inf.txt;
                return false;
            }
            --index;
            return true;
        }
    } s(index);

    iterate_items(s, s);
    return s.t;
}

/* may never need

menu_c& menu_c::decode_and_add(const ts::wsptr & encoded)
{
    ts::token<ts::wchar> t(encoded, L'\1');
    if (curbp == nullptr) prepare();
    ts::wbp_c &item = curbp->addBlockPar(*t);
    item.set_value(t.tail());
    return *this;
}

void menu_c::operator<<(const ts::wstrings_c &sarr)
{
    for(const ts::wstr_c &s : sarr)
    {
        decode_and_add( s );
    }
}

void menu_c::operator>>(ts::wstrings_c &sarr)
{
    struct saver
    {
        ts::wstrings_c &sarr;
        saver(ts::wstrings_c &sarr):sarr(sarr) {}
        bool operator()(saver&, const ts::wsptr&) {return true;} // skip separator
        bool operator()(saver&, const ts::wsptr&, const menu_c&) {return true;} // skip submenu
        bool operator()(saver&, const ts::wstr_c&txt, MENUHANDLER h, const ts::str_c&prm)
        {
            sarr.add( menu_c::encode(txt,h,prm) );
            return true;
        }
    private:
        void operator=(const saver &) UNUSED;
    } s(sarr);

    iterate_items(s, s);
}
*/