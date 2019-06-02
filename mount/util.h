#ifndef UTIL_H
#define UTIL_H

#include "define.h"

#include <string>
#include <vector>
#include <cassert>
#include <algorithm>

using std::string;
using std::vector;

namespace util
{
    class Bitmap
    {
    public:
        Bitmap(unsigned int max = 8)
        {
            resize(max);
        }

        Bitmap(const Bitmap&) = delete;
        Bitmap& operator = (const Bitmap&) = delete;

        unsigned int capacity()
        {
            return max_;
        }

        void resize(unsigned int max)
        {
            if (max == 0)
                return;

            max_ = max;

            bitmap_.resize(ceil(max / (8 * sizeof(unsigned int))), 0);
            //清零
            std::transform(begin(bitmap_), end(bitmap_), begin(bitmap_),
                [](unsigned int val){ return 0; });
        }

        //i >> 5相当于i / 32
        //i & 31相当于i % 32

        void set(unsigned int i)
        {
            if (i >= max_)
                return;

            bitmap_[i >> 5] |= 1 << (i & 31);
        }

        int get(unsigned int i)
        {
            if (i >= max_)
                return 0;

            return bitmap_[i >> 5] & (1 << (i & 31));
        }

        void clear(unsigned int i)
        {
            if (i >= max_)
                return;

            bitmap_[i >> 5] & ~(1 << (i & 31));
        }
        
    private:
        vector<unsigned int> bitmap_;
        unsigned int max_;  
    };

    string transform(const string& path);
    bool mkdir(const string& path);
    bool mkfile(const string& path, const vector<byte>& buf);
    bool rmdir(const string& path);
    bool rmfile(const string& path);
}

#endif//UTIL_H