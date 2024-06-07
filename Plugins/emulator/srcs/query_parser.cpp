#include "query_parser.h"
#include <cmath>
#include <typeinfo>

struct intcompare
{

    bool operator()(int &a, int &b)
    {
        if(std::less()(a, b)) return true;
        else return false;
    }
};

struct doublecompare
{
    bool operator()(double &a, double &b)
    {
        if(std::isless(a, b)) return true;
        else return false;
    }

};

struct stringcompare
{
    bool operator()(std::string &a, std::string &b)
    {
        if(a.compare(b) < 0) return true;
        else return false;
    }
};


template <typename T, class CompareFcn1, class CompareFcn2=stringcompare>
bool compare_events(view &e1, view &e2)
{
    int offset1 = e1.offset;
    int len1 = e1.length;
    int offset2 = e2.offset;
    int len2 = e2.length;

    T v1, v2;
    std::string t;

    if(typeid(v1) == typeid(t))
    {
        std::string s1, s2;
        s1.assign(&(e1.e.data[offset1]), len1);
        s2.assign(&(e2.e.data[offset2]), len2);
        if(CompareFcn2()(s1, s2)) return true;
        else return false;
    }
    else
    {
        memcpy(&v1, &e1.e.data[offset1], len1);
        memcpy(&v2, &e2.e.data[offset2], len2);
        if(CompareFcn1()(v1, v2)) return true;
        else return false;
    }
}

bool compare_events_string(view &e1, view &e2)
{
    int offset1 = e1.offset;
    int len1 = e1.length;
    int offset2 = e2.offset;
    int len2 = e2.length;

    std::string v1, v2;
    v1.assign(&(e1.e.data[offset1]), len1);
    v2.assign(&(e2.e.data[offset2]), len2);

    if(stringcompare()(v1, v2)) return true;
    else return false;

}

bool query_parser::sort_events_by_attr(std::string &s, std::vector <struct event> &r_events, std::string &a_name
                                       , event_metadata &em, std::vector <view> &resp)
{
    bool b;
    int offset;
    b = em.get_offset(a_name, offset);
    int size;
    b = em.get_size(a_name, size);

    for(int i = 0; i < r_events.size(); i++)
    {
        int r = random() % 1000;
        memcpy(&r_events[i].data[offset], &r, sizeof(int));
    }

    for(int i = 0; i < r_events.size(); i++)
    {
        view ep;
        ep.offset = offset;
        ep.length = sizeof(int);
        ep.e = r_events[i];
        resp.push_back(ep);
    }

    std::sort(resp.begin(), resp.end(), compare_events <int, intcompare, stringcompare>);

    return true;
}

template <typename T, class EqualFcn=std::equal_to <T>>
bool query_parser::select_by_attr(std::string &s, std::string &a_name, std::vector <struct event> &r_events
                                  , event_metadata &em, T &value, std::vector <view> &resp)
{

    int offset = 0;
    bool b = em.get_offset(a_name, offset);
    int size = 0;
    b = em.get_size(a_name, size);

    for(int i = 0; i < r_events.size(); i++)
    {
        T v;
        memcpy(&v, &r_events[i].data[offset], size);
        if(EqualFcn()(v, value))
        {
            view ep;
            ep.offset = offset;
            ep.length = size;
            ep.e = r_events[i];
            resp.push_back(ep);
        }
    }
    return true;
}

bool
query_parser::create_view_from_events(std::string &s, std::vector <struct event> &r_events, std::vector <view> &resp)
{

    for(int i = 0; i < r_events.size(); i++)
    {
        view ep;
        ep.offset = 0;
        ep.length = sizeof(uint64_t);
        ep.e = r_events[i];
        resp.push_back(ep);
    }
    return true;
}

bool query_parser::add_view_to_cache(std::string &s, std::string &a_name, std::vector <view> &v)
{

    std::string vname = s + ":" + a_name;
    auto r = cached_views.find(vname);
    auto t = lookup_tables.find(vname);

    std::unordered_map <uint64_t, int> mytable;

    if(t == lookup_tables.end())
    {
        std::pair <std::string, std::unordered_map <uint64_t, int>> p;
        p.first = vname;
        for(int i = 0; i < v.size(); i++)
        {
            uint64_t key = v[i].e.ts;
            std::pair <uint64_t, int> p1(key, i);
            p.second.insert(p1);
        }
        lookup_tables.insert(p);
    }
    else
    {
        for(int i = 0; i < v.size(); i++)
        {
            uint64_t key = v[i].e.ts;
            std::pair <uint64_t, int> p1(key, i);
            t->second.insert(p1);
        }
    }

    if(r == cached_views.end())
    {

        std::pair <std::string, std::vector <view>*> p;
        p.first = vname;
        p.second = new std::vector <view>();
        p.second->assign(v.begin(), v.end());
        cached_views.insert(p);
    }
    else
    {
        r->second->assign(v.begin(), v.end());
    }
}
