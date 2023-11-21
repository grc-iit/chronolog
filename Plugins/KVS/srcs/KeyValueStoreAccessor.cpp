template <typename T, typename N>
int KeyValueStoreAccessor::add_new_inverted_list(std::string &table, std::string &attr_name, int size, int ntables
                                                 , N &emptykey, data_server_client*d, KeyValueStoreIO*io
                                                 , Interface_Queues*q, int c, int data_size)
{
    T*invlist = new T(numprocs, myrank, size, ntables, emptykey, table, attr_name, d, io, q, c, data_size);

    std::string type = md.get_type(attr_name);
    int keytype = 0;
    if(type.compare("int") == 0) keytype = 0;
    else if(type.compare("unsignedlong") == 0) keytype = 1;
    else if(type.compare("float") == 0) keytype = 2;
    else if(type.compare("double") == 0) keytype = 3;

    kio->add_query_service(keytype, (void*)invlist);
    invlist->bind_functions();

    std::pair <std::string, void*> sp;
    sp.first = attr_name;
    sp.second = (void*)invlist;
    lists.push_back(sp);
    std::pair <std::string, int> ip;
    ip.first = attr_name;
    ip.second = lists.size() - 1;
    secondary_attributes.insert(ip);

    return lists.size() - 1;
}

template <typename T>
bool KeyValueStoreAccessor::delete_inverted_list(int n)
{
    if(n < lists.size())
    {
        T*invlist = reinterpret_cast<T*>(lists[n].second);
        delete invlist;
        return true;
    }
    return false;
}

template <typename T, typename N>
bool KeyValueStoreAccessor::insert_entry(int pos, N &key, uint64_t &ts)
{
    if(pos >= lists.size()) return false;

    T*invlist = reinterpret_cast<T*>(lists[pos].second);
    bool b = invlist->put_entry(key, ts);
    return b;
}

template <typename T, typename N, typename M>
uint64_t KeyValueStoreAccessor::Put_ts(int pos, std::string &s, N &key, M &value)
{
    if(pos >= lists.size()) return UINT64_MAX;
    int ksize = sizeof(N);
    std::string data = value;
    assert(data.length() == value.length());

    std::vector <uint64_t> ts;

    do
    {
        ts = if_q->PutEmulatorEvent(s, data, myrank);
    } while(ts[0] == UINT64_MAX && ts[1] == 0);

    bool b = false;
    if(ts[0] != UINT64_MAX)
    {
        T*invlist = reinterpret_cast<T*>(lists[pos].second);
        b = invlist->put_entry(key, ts[0]);
    }
    return ts[0];
}

template <typename T, typename N, typename M>
bool KeyValueStoreAccessor::Put(int pos, std::string &s, N &key, M &value)
{
    if(pos >= lists.size()) return false;

    int ksize = sizeof(N);

    std::string data = value;

    assert(data.length() == value.length());

    std::vector <uint64_t> ts;
    do
    {
        ts = if_q->PutEmulatorEvent(s, data, myrank);
    } while(ts[0] == UINT64_MAX && ts[1] == 0);

    bool b = false;
    if(ts[0] != UINT64_MAX)
    {
        T*invlist = reinterpret_cast<T*>(lists[pos].second);
        b = invlist->put_entry(key, ts[0]);
        if(b) inserts++;
        return true;
    }
    else if(ts[1] == 2) return false;
    else return true;
}


template <typename T, typename N>
bool KeyValueStoreAccessor::Get(int pos, std::string &s, N &key, int id)
{
    if(pos >= lists.size()) return false;

    uint64_t ts = UINT64_MAX;
    bool b = false;
    std::vector <uint64_t> values;

    T*invlist = reinterpret_cast<T*>(lists[pos].second);
    int pid = invlist->get_entry(key, values);

    if(values.size() > 0)
    {
        ts = values[values.size() - 1];
        std::string eventstring = if_q->GetEmulatorEvent(s, ts, myrank);
        if(eventstring.length() != 0)
            invlist->add_event_file(eventstring);
        else
        {
            bool bp = false;
            invlist->AddPending(key, values, bp, id, pid);
        }
    }
    else
    {
        pid = invlist->partition_no(key);
        if(!invlist->CheckLocalFileExists())
        {
            std::string filename = "file";
            filename += s + ".h5";
            if(if_q->CheckFileExistence(filename, myrank))
            {
                invlist->LocalFileExists();
            }
        }
        bool bp = false;
        invlist->AddPending(key, values, bp, id, pid);

    }
    return true;

}

template <typename T, typename N>
bool KeyValueStoreAccessor::Get_resp(int pos, std::string &s, N &key, int id)
{
    if(pos >= lists.size()) return false;
    uint64_t ts = UINT64_MAX;
    bool b = false;
    std::vector <uint64_t> values;

    T*invlist = reinterpret_cast<T*>(lists[pos].second);
    int pid = invlist->get_entry(key, values);
    if(values.size() > 0)
    {
        ts = values[values.size() - 1];
        std::string eventstring = if_q->GetEmulatorEvent(s, ts, myrank);
        if(eventstring.length() == 0)
        {
            if(!invlist->CheckLocalFileExists())
            {
                std::string filename = "file";
                filename += s + ".h5";
                if(if_q->CheckFileExistence(filename, myrank))
                {
                    invlist->LocalFileExists();
                }
            }
            bool bp = true;

            invlist->AddPending(key, values, bp, id, pid);
        }
        else invlist->add_response(key, eventstring, id);

    }
    else
    {
        pid = invlist->partition_no(key);

        if(!invlist->CheckLocalFileExists())
        {
            std::string filename = "file";
            filename += s + ".h5";
            if(if_q->CheckFileExistence(filename, myrank))
            {
                invlist->LocalFileExists();
            }
        }
        bool bp = true;
        invlist->AddPending(key, values, bp, id, pid);
    }

    return true;
}

template <typename T, typename N>
std::vector <std::pair <int, std::string>> KeyValueStoreAccessor::Completed_Gets(int pos, std::string &s)
{
    std::vector <std::pair <int, std::string>> ids;
    if(pos >= lists.size()) return ids;

    T*invlist = reinterpret_cast<T*>(lists[pos].second);
    ids = invlist->get_completed_ids();
    return ids;
}

template <typename T, typename N>
std::vector <uint64_t> KeyValueStoreAccessor::get_entry(int pos, N &key)
{
    std::vector <uint64_t> values;
    if(pos >= lists.size()) return values;

    T*invlist = reinterpret_cast<T*>(lists[pos].second);
    int ret = invlist->get_entry(key, values);
    return values;
}

template <typename T>
void KeyValueStoreAccessor::flush_invertedlist(std::string &attr_name, bool p)
{
    int offset = md.locate_offset(attr_name);

    std::string tname = md.db_name();

    if(offset == -1) return;

    int pos = get_inverted_list_index(attr_name);

    if(pos == -1) return;

    T*invlist = reinterpret_cast<T*>(lists[pos].second);

    if(!invlist->CheckLocalFileExists())
    {
        std::string filename = "file";
        filename += tname + ".h5";
        if(if_q->CheckFileExistence(filename, myrank))
        {
            invlist->LocalFileExists();
        }
    }

    struct sync_request*r = new struct sync_request();
    std::string type = md.get_type(attr_name);
    int keytype = 0;
    if(type.compare("int") == 0) keytype = 0;
    else if(type.compare("unsignedlong") == 0) keytype = 1;
    else if(type.compare("float") == 0) keytype = 2;
    else if(type.compare("double") == 0) keytype = 3;

    r->funcptr = (void*)invlist;
    r->name = md.db_name();
    r->attr_name = attr_name;
    r->offset = offset;
    r->keytype = keytype;
    r->flush = true;
    r->persist = p;

    bool ret = kio->LocalPutSyncRequest(r);

}
