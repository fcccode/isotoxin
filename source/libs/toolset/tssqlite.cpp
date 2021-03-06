#include "toolset.h"
#ifndef SQLITE_HAS_CODEC
#define SQLITE_HAS_CODEC 1
#endif
#include "sqlite3/sqlite3.h"

namespace ts
{

namespace
{
struct sqlite_encrypt_stuff_s
{
    SQLITE_ENCRYPT_PROCESS_CALLBACK cb;
};
static sqlite_encrypt_stuff_s *estuff = nullptr;

struct getdacolumn
{
    sqlite3_stmt *stmt;

    data_type_e get_da_value( int index, data_value_s &v )
    {
        int ct = sqlite3_column_type( stmt, index );
        switch (ct)
        {
        case SQLITE_INTEGER:
            v.i = sqlite3_column_int64( stmt, index );
            return data_type_e::t_int;
        case SQLITE_FLOAT:
            v.f = sqlite3_column_double( stmt, index );
            return data_type_e::t_float;
        case SQLITE3_TEXT:
            v.text.set( asptr( (const char *)sqlite3_column_text( stmt, index ) ) );
            return data_type_e::t_str;
        case SQLITE_BLOB:
            v.blob.set_size( sqlite3_column_bytes( stmt, index ) );
            memcpy( v.blob.data(), sqlite3_column_blob( stmt, index ), v.blob.size() );
            return data_type_e::t_blob;
        case SQLITE_NULL:
            v.blob.clear();
            v.text.clear();
            v.i = 0;
            return data_type_e::t_null;
        default:
            FORBIDDEN();
        };

        return data_type_e::t_null;
    }
};

class sqlite3_c : public sqlitedb_c
{
    sqlite3 *db = nullptr;
    int transaction_ref = 0;
    bool readonly = false;
public:
    sqlite3_c( bool readonly ):readonly(readonly) {}
    ts::wstr_c fn;

    /*virtual*/ void close() override
    {
        if (ASSERT(db))
        {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    static int callback(void *NotUsed, int argc, char **argv, char **azColName)
    {
        int i;
        for (i = 0; i < argc; i++)
        {
            WARNING("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
        }
        return 0;
    }

    void execsql( const tmp_str_c &sql )
    {
        char *zErrMsg = nullptr;
        sqlite3_exec(db, sql, callback, 0, &zErrMsg);
        if (zErrMsg) 
        {
            WARNING(zErrMsg);
            sqlite3_free(zErrMsg);
        }
    }

    /*virtual*/ void begin_transaction() override
    {
        if (++transaction_ref == 1)
        {
            execsql(tmp_str_c("BEGIN TRANSACTION"));
        }
    }

    /*virtual*/ void end_transaction() override
    {
        ASSERT(transaction_ref > 0);
        if (--transaction_ref == 0)
        {
            execsql(tmp_str_c("END TRANSACTION"));
        }
    }


    /*virtual*/ bool is_correct() const override
    {
        if (!db)
            return false;

        tmp_str_c tstr;
        streamstr<tmp_str_c> sql(tstr);
        sql << "SELECT name FROM sqlite_master WHERE type='table'";

        sqlite3_stmt *stmt;
        int erc = sqlite3_prepare_v2(db, sql.buffer(), (int)sql.buffer().get_length(), &stmt, nullptr);
        if (erc != SQLITE_OK) return false;
        int step = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return SQLITE_ROW == step || SQLITE_DONE == step;
    }

    /*virtual*/ bool is_table_exist( const asptr& tablename ) override
    {
        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            //sql << CONSTASTR("SELECT name FROM sqlite_master WHERE type=\'table\' AND name=\'") << tablename << "\'";
            sql << "PRAGMA table_info(" << tablename << ")";
            
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, sql.buffer(), (int)sql.buffer().get_length(), &stmt, nullptr);
            int step = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return SQLITE_ROW == step;
        }
        return false;
    }

    /*virtual*/ void create_table( const asptr& tablename, array_wrapper_c<const column_desc_s> columns, bool norowid ) override
    {
        if (readonly) return;
        if (ASSERT(db))
        {
            execsql( tmp_str_c(CONSTASTR("drop table if exists "), tablename) );

            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("CREATE TABLE `") << tablename << CONSTASTR("` (");
            for (const column_desc_s& c : columns)
            {
                sql << "`" << c.name_ << "` ";
                switch (c.type_)
                {
                case data_type_e::t_int:
                case data_type_e::t_int64:
                    sql << CONSTASTR("integer ");
                    break;
                case data_type_e::t_float:
                    sql << CONSTASTR("float");
                    break;
                case data_type_e::t_str:
                    sql << CONSTASTR("text");
                    break;
                case data_type_e::t_blob:
                    sql << CONSTASTR("blob");
                    break;
                default:
                    ERROR("unknown columt type");
                    break;
                }
                if (c.options.is(column_desc_s::f_primary)) sql << CONSTASTR("PRIMARY KEY");
                if (c.options.is(column_desc_s::f_autoincrement)) sql << CONSTASTR("AUTOINCREMENT");
                if (!c.options.is(column_desc_s::f_nullable) && c.type_ != data_type_e::t_blob) sql << CONSTASTR("NOT NULL");
                if (c.has_default()) sql << CONSTASTR("default ") << c.get_default();
                sql << CONSTASTR(",");
            }
            sql.buffer().trunc_length();
            sql << CONSTASTR(")");
            if (norowid) sql << CONSTASTR("WITHOUT ROWID");
            
            execsql(sql.buffer());
        }
    }

    void create_index( const asptr& tablename, array_wrapper_c<const column_desc_s> columns ) //-V813
    {
        if (readonly) return;
        tmp_str_c sql;
        for(const column_desc_s &cd : columns)
        {
            if (cd.options.is(column_desc_s::f_unique_index | column_desc_s::f_non_unique_index))
            {
                sql.clear();
                sql.append(CONSTASTR("CREATE "));
                if (cd.options.is(column_desc_s::f_unique_index)) sql.append(CONSTASTR("UNIQUE "));
                sql.append(CONSTASTR("INDEX IF NOT EXISTS index_")).append(cd.name_);
                sql.append(CONSTASTR(" ON ")).append(tablename);
                sql.append(CONSTASTR(" (")).append(cd.name_).append_char(')');
                execsql(sql);
            }
        }
    }


    /*virtual*/ int update_table_struct(const asptr& tablename, array_wrapper_c<const column_desc_s> columns, bool norowid) override
    {
        MEMT( MEMT_SQLITE );

        if (!is_table_exist(tablename))
        {
            if (readonly) return -1;
            execsql(tmp_str_c(CONSTASTR("BEGIN")));
            create_table(tablename,columns,norowid);
            create_index(tablename,columns);
            execsql(tmp_str_c(CONSTASTR("COMMIT")));
            return 1;
        }

        if (ASSERT(db))
        {
            bool recreate = false;
            sstr_c tbln(tablename); // asptr -> zero-end-string
            int cid = 0;
            tmp_str_c sc;
            for(const column_desc_s&cd : columns)
            {
                const char *datatype, *collseq;
                int notnull,primarykey,autoinc, index;
                recreate |= SQLITE_ERROR == sqlite3_table_column_metadata(db,nullptr,tbln,tmp_str_c(cd.name_),&datatype,&collseq,&notnull,&primarykey,&autoinc,&index);

                if (datatype == nullptr)
                {
                    if (cd.options.is(column_desc_s::f_nullable) || cd.type_ == data_type_e::t_blob)
                        sc.append(',', CONSTASTR("NULL") );
                    else
                        sc.append(',', cd.get_default().as_sptr() );
                    sc.append(CONSTASTR(" as ")).append(cd.name_);
                } else
                {
                    sc.append(',', cd.name_);
                }

                if (recreate) continue;
                if (cid != index) { recreate = true; continue; }
                if (cd.type_ != data_type_e::t_blob) if ((notnull==0) != cd.options.is(column_desc_s::f_nullable)) { recreate = true; continue; }
                if ((primarykey!=0) != cd.options.is(column_desc_s::f_primary)) { recreate = true; continue; }
                if ((autoinc!=0) != cd.options.is(column_desc_s::f_autoincrement)) { recreate = true; continue; }
                switch (cd.type_)
                {
                case data_type_e::t_int:
                case data_type_e::t_int64:
                    if (!pstr_c(CONSTASTR("integer")).equals(asptr(datatype))) recreate = true;
                    break;
                case data_type_e::t_float:
                    DEBUG_BREAK();
                    break;
                case data_type_e::t_str:
                    if (!pstr_c(CONSTASTR("text")).equals(asptr(datatype))) recreate = true;
                    break;
                case data_type_e::t_blob:
                    if (!pstr_c(CONSTASTR("blob")).equals(asptr(datatype))) recreate = true;
                    break;
                }
                ++cid;
            }
            
            if (recreate)
            {
                if (readonly) return -1;
                execsql(tmp_str_c(CONSTASTR("BEGIN")));
                tbln.insert(0,CONSTASTR("new__"));
                create_table(tbln,columns,norowid);
            
                tmp_str_c tstr;
                streamstr<tmp_str_c> sql(tstr);
                sql << CONSTASTR("INSERT INTO `") << tbln << CONSTASTR("` SELECT ") << sc << CONSTASTR(" FROM `") << tablename << "`";
                
                execsql(sql.buffer());
                execsql( tmp_str_c(CONSTASTR("drop table "), tablename) );

                sql.buffer().clear();
                sql << CONSTASTR("ALTER TABLE `") << tbln << CONSTASTR("` RENAME TO `") << tablename << "`";
                execsql(sql.buffer());
                create_index(tablename, columns);
                execsql(tmp_str_c(CONSTASTR("COMMIT")));
            } else
            {
                if (readonly) return 0;
                execsql(tmp_str_c(CONSTASTR("BEGIN")));
                create_index(tablename, columns);
                execsql(tmp_str_c(CONSTASTR("COMMIT")));
            }
        }
        return 0;
    }

    /*virtual*/ int  count( const asptr& tablename, const asptr& where_items ) override
    {
        int cnt = 0;
        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("select count(rowid) from \'") << tablename << CONSTASTR("\' where ") << where_items;

            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, sql.buffer(), (int)sql.buffer().get_length(), &stmt, nullptr);
            int step = sqlite3_step(stmt);
            if (SQLITE_ROW == step)
                cnt = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);

        }
        return cnt;
    }

    /*virtual*/ int find_free( const asptr& tablename, const asptr& id ) override
    {
        tmp_str_c tstr;
        streamstr<tmp_str_c> sql(tstr);
        sql << CONSTASTR("select `") << id << CONSTASTR("` from \'") << tablename << CONSTASTR("\' order by `") << id << CONSTASTR("`");

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, sql.buffer(), (int)sql.buffer().get_length(), &stmt, nullptr);

        int idcheck = 1;

        for (;SQLITE_ROW == sqlite3_step(stmt);)
        {
            int getid = sqlite3_column_int(stmt, 0);
            if (getid > idcheck)
                break;
            ++idcheck;
        }
        sqlite3_finalize(stmt);
        return idcheck;
    }

    /*virtual*/ int insert( const asptr& tablename, array_wrapper_c<const data_pair_s> fields ) override
    {
        if (readonly) return -1;

        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("insert or replace into \'") << tablename << CONSTASTR("\' (");
            for( const data_pair_s &d : fields )
                sql << d.name << CONSTASTR(",");
            sql.buffer().trunc_length();
            sql << CONSTASTR(") values (");
            for (int i=0;i<fields.size();++i)
                sql << CONSTASTR("?,");
            sql.buffer().trunc_length();
            sql << CONSTASTR(")");

            int lastid = 0;
            sqlite3_stmt *insstmt = nullptr;
            int prepr = sqlite3_prepare_v2(db, sql.buffer(), -1, &insstmt, nullptr);
            if (!CHECK( SQLITE_OK == prepr, "" << sqlite3_extended_errcode(db) )) return 0;
            aint cnt = fields.size();
            ASSERT(cnt == sqlite3_bind_parameter_count(insstmt));
            for(int i=1;i<=cnt;++i)
            {
                const data_pair_s &d = fields[i-1];

                switch (d.type_)
                {
                    case data_type_e::t_int:
                        sqlite3_bind_int(insstmt, i, (int)d.i);
                        break;
                    case data_type_e::t_int64:
                        sqlite3_bind_int64(insstmt, i, d.i);
                        break;
                    case data_type_e::t_str:
                        sqlite3_bind_text(insstmt, i, d.text.cstr(), (int)d.text.get_length(), SQLITE_STATIC);
                        break;
                    case data_type_e::t_blob:
                        sqlite3_bind_blob(insstmt, i, d.blob.data(), (int)d.blob.size(), SQLITE_STATIC);
                        break;
                    default:
                        FORBIDDEN();
                        break;
                }

            }
            int r =sqlite3_step(insstmt);
            if (CHECK(SQLITE_DONE == r, "" << sqlite3_extended_errcode(db)))
                lastid = (int)sqlite3_last_insert_rowid(db);

            sqlite3_finalize(insstmt);

            return lastid;
        }
        return 0;
    }

    /*virtual*/ void update(const asptr& tablename, array_wrapper_c<const data_pair_s> fields, const asptr& where_items) override
    {
        if (readonly) return;

        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("update \'") << tablename << CONSTASTR("\' set ");
            for (const data_pair_s &d : fields)
                sql << d.name << CONSTASTR("=?,");
            sql.buffer().trunc_length();
            if (where_items.l) sql << CONSTASTR(" where ") << where_items;

            sqlite3_stmt *updstmt = nullptr;
            if (!CHECK(SQLITE_OK == sqlite3_prepare_v2(db, sql.buffer(), -1, &updstmt, nullptr))) return;
            aint cnt = fields.size();
            ASSERT(cnt == sqlite3_bind_parameter_count(updstmt));
            for (aint i = 1; i <= cnt; ++i)
            {
                const data_pair_s &d = fields[i - 1];

                switch (d.type_)
                {
                    case data_type_e::t_int:
                        sqlite3_bind_int(updstmt, (int)i, (int)d.i);
                        break;
                    case data_type_e::t_int64:
                        sqlite3_bind_int64(updstmt, (int)i, d.i);
                        break;
                    case data_type_e::t_str:
                        sqlite3_bind_text(updstmt, (int)i, d.text.cstr(), (int)d.text.get_length(), SQLITE_STATIC);
                        break;
                    case data_type_e::t_blob:
                        sqlite3_bind_blob(updstmt, (int)i, d.blob.data(), (int)d.blob.size(), SQLITE_STATIC);
                        break;
                    default:
                        FORBIDDEN();
                        break;
                }

            }

            for (; SQLITE_ROW == sqlite3_step(updstmt);) ;
            
            sqlite3_finalize(updstmt);
        }
    }

    /*virtual*/ void delrow( const asptr& tablename, int id ) override
    {
        if (readonly) return;

        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("delete from \'") << tablename << CONSTASTR("\' where id=") << id;

            execsql( sql.buffer() );
        }
    }

    /*virtual*/ void  delrows(const asptr& tablename, const asptr& where_items) override
    {
        if (readonly) return;
        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("delete from \'") << tablename << CONSTASTR("\' where ") << where_items;
            execsql(sql.buffer());
        }
    }

    /*virtual*/ bool unique_values( const asptr& tablename, SQLITE_UNIQUE_VALUES reader, const asptr& column )
    {
        if (ASSERT( db ))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql( tstr );
            sql << CONSTASTR( "SELECT `" ) << column << CONSTASTR( "` FROM `" ) << tablename << CONSTASTR( "` group by `" ) << column << CONSTASTR( "`" );

            struct getdacolumn0 : public getdacolumn
            {
                SQLITE_UNIQUE_VALUES reader;

                getdacolumn0( SQLITE_UNIQUE_VALUES reader ) :reader( reader ) {}
                ~getdacolumn0()
                {
                    for (int row = 0; SQLITE_ROW == sqlite3_step( stmt ); ++row)
                    {
                        data_value_s v;
                        get_da_value( 0, v );
                        reader( v );
                    }
                    sqlite3_finalize( stmt );
                }
            } columngetter( reader );

            return SQLITE_OK == sqlite3_prepare_v2( db, sql.buffer(), (int)sql.buffer().get_length(), &columngetter.stmt, nullptr );
        }
        return false;
    }

    /*virtual*/ bool read_table( const asptr& tablename, SQLITE_TABLEREADER reader, const asptr& where_items ) override
    {
        if (ASSERT(db))
        {
            tmp_str_c tstr;
            streamstr<tmp_str_c> sql(tstr);
            sql << CONSTASTR("SELECT * FROM \'") << tablename << "\'";
            if (where_items.l)
                sql << CONSTASTR(" where ") << where_items;

            struct getdacolumn0 : public getdacolumn
            {
                SQLITE_TABLEREADER reader;

                getdacolumn0( SQLITE_TABLEREADER reader ):reader(reader) {}
                ~getdacolumn0()
                {
                    auto getta = DELEGATE( this, get_da_value );
                    for( int row = 0; SQLITE_ROW == sqlite3_step(stmt); ++row )
                    {
                        if (!reader( row, getta )) break;

                    }
                    sqlite3_finalize(stmt);
                }
            } columngetter(reader);

            return SQLITE_OK == sqlite3_prepare_v2( db, sql.buffer(), (int)sql.buffer().get_length(), &columngetter.stmt, nullptr );
        }
        return false;
    }

    /*virtual*/ void rekey(const uint8 *k, SQLITE_ENCRYPT_PROCESS_CALLBACK cb) override
    {
        // only one rekey allowed

        if (readonly || estuff != nullptr)
        {
            cb( 0, 0 );
            return;
        }
        readonly = true; // disallow any modifications during encrypt

        sqlite_encrypt_stuff_s stuff;
        stuff.cb = cb;
        estuff = &stuff;

        sqlite3_rekey(db, k, k ? 48 : 0);

        estuff = nullptr;
        readonly = false;
    }

    bool inactive() const {return db == nullptr;}
    void open(const wsptr &fn_, const uint8 *passhash)
    {
        fn = fn_;
        ts::str_c utf8name = to_utf8( fn );
        sqlite3_open_v2(utf8name, &db, SQLITE_OPEN_FULLMUTEX | (readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)), nullptr);
        if ( passhash )
            sqlite3_key(db, passhash, 48);
    }
};

class sqlite_init
{
    array_inplace_t< UNIQUE_PTR(sqlite3_c), 0 > dbs;
public:
    sqlite_init()
    {
        sqlite3_initialize();
    }
    ~sqlite_init()
    {
        DMSG("~sqlite_init");
        sqlite3_shutdown();
    }

    sqlitedb_c *get( const wsptr &fn, const uint8 *passhash, bool readonly )
    {
        MEMT( MEMT_SQLITE );

        ts::wstr_c fnn(fn);
        ts::fix_path(fnn, FNO_LOWERCASEAUTO | FNO_NORMALIZE);
        UNIQUE_PTR(sqlite3_c) *recruit = nullptr;
        for (UNIQUE_PTR(sqlite3_c) &ptr : dbs)
        {
            if (ptr.get() == nullptr || ptr.get()->inactive())
            {
                if (recruit == nullptr)
                    recruit = &ptr;
                continue;
            }
            if (ptr->fn == fnn) return ptr.get();
        }
        
        if (recruit == nullptr) recruit = &dbs.add();
        if (recruit->get() == nullptr) recruit->reset( TSNEW(sqlite3_c, readonly) );
        recruit->get()->open(fnn, passhash);
        if (!recruit->get()->inactive()) return recruit->get();
        return nullptr;
    }
};

static_setup<sqlite_init> sqliteinit;

}


sqlitedb_c *sqlitedb_c::connect(const wsptr &fn, const uint8 *k, bool readonly)
{
    return sqliteinit().get(fn, k, readonly);
}

}

extern "C"
{
    void rekey_process_callback(const void *zKey, int nKey, int i, int n)
    {
        if (ts::estuff)
            ts::estuff->cb( i, n );
    }
}
