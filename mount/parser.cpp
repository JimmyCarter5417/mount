#include "parser.h"
#include "define.h"

#include <cassert>
#include <queue>

Parser::Parser(const string& src, const string& dst)
: src_(util::transform(src))
, dst_(util::transform(dst))
, fp_(nullptr)
{
    pcache_[2] = "/";//根目录inode号固定为2，必须事先指定
}

Parser::~Parser()
{
    if (fp_)
    {
        fclose(fp_);
        fp_ = nullptr;
        
    }
}

//忽略lost+found
bool Parser::isValidInode(uint inode)
{
    return inode == 2 || inode >= 12;
}

//offset若为int最大只支持2G
bool Parser::read(void* buf, uint len, uint offset)
{
    if (!fp_)
        fp_ = fopen(src_.c_str(), "rb");

    if (!fp_)
        return false;

    if (offset > 0)
        fseek(fp_, offset, SEEK_SET);

    return fread(buf, 1, len, fp_);
}

bool Parser::getFsInfo()
{
    ext4_super_block sb = { 0 };
    if (!read(&sb, sizeof sb, GROUP_0_PADDING))
        return false;

    assert(sb.s_magic == 0xEF53);
    assert(sb.s_feature_incompat & INCOMPAT_EXTENTS);
    assert(sb.s_feature_incompat & INCOMPAT_FLEX_BG);
    assert(sb.s_feature_incompat & INCOMPAT_FILETYPE);
    assert(!(sb.s_feature_incompat & INCOMPAT_META_BG));
    assert(!(sb.s_feature_incompat & INCOMPAT_64BIT));
    assert(!(sb.s_feature_incompat & INCOMPAT_INLINE_DATA));
    assert(sb.s_feature_ro_compat & RO_COMPAT_SPARSE_SUPER);

    fs_.block_count = sb.s_blocks_count_lo;
    fs_.block_size = pow(2, 10 + sb.s_log_block_size);
    assert(fs_.block_size > 1024);
    fs_.inode_count = sb.s_inodes_count;
    fs_.inode_size = sb.s_inode_size;
    fs_.blocks_per_group = sb.s_blocks_per_group;
    fs_.inodes_per_group = sb.s_inodes_per_group;
    fs_.groups_per_flex = pow(2, sb.s_log_groups_per_flex);

    assert(fs_.blocks_per_group);
    fs_.group_count = ceil(1.0 * fs_.block_count / fs_.blocks_per_group);
    assert(fs_.groups_per_flex);
    fs_.flex_count = ceil(1.0 * fs_.group_count / fs_.groups_per_flex);

    return true;
}

bool Parser::getGds()
{
    for (int i = 0; i < fs_.flex_count; ++i)
    {
        //flex块组的第一个块组的第一个逻辑块保存的是superblock，必须注意
        uint offset = fs_.block_size * fs_.blocks_per_group * fs_.groups_per_flex * i + fs_.block_size;

        for (int j = 0; j < fs_.groups_per_flex; ++j)
        {
            if (gds_.size() == fs_.group_count)
                break;

            ext4_group_desc gd = { 0 };
            read(&gd, sizeof gd, offset);
            gds_.push_back(gd);

            offset += sizeof gd;
        }
    }

    return true;
}

bool Parser::getInodeBitmap()
{
    bmAllInode_.resize(fs_.inode_count);
    
    int count = 0;
    int cur = 0;
    for (int i = 0; i < gds_.size(); ++i)
    {
        if (gds_[i].bg_inode_bitmap_lo)
        {
            //读取第i个块组的inode位图所在的块
            vector<byte> buf(fs_.block_size);
            int offset = gds_[i].bg_inode_bitmap_lo * fs_.block_size;
            read(buf.data(), buf.size(), offset);
            //从块中提取inode位图数据
            for (int j = 0; j < fs_.inodes_per_group; ++j)
            {
                if (buf[j >> 3] & (1 << (j & 7)))
                {
                    bmAllInode_.set(cur);
                    count++;
                }   
                else
                {
                    bmAllInode_.clear(cur);
                }   

                ++cur;
            }
        }
        else
        {
            //未使用的块组inode位图一律清零 应该不会出现
            for (int end = cur + fs_.inodes_per_group; cur < end; ++cur)
            {
                bmAllInode_.clear(cur);
            }
        }
    }

    return true;
}

bool Parser::getInodes()
{
    //检测0号inode无意义
    for (int i = 0; i < fs_.inode_count; ++i)
    {
        if (bmAllInode_.get(i))
        {
            uint gdIdx = i / fs_.inodes_per_group;//inode所在的块组
            int offset = fs_.inode_size * ((i - 1) % fs_.inodes_per_group)//inode在块组inode表的内部偏移
                       + gds_[gdIdx].bg_inode_table_lo * fs_.block_size;//块组inode表位置
            
            ext4_inode inode = {0};
            read(&inode, sizeof inode, offset);
            icache_[i] = inode;
        }
    }

    return true;
}

bool Parser::getData()
{   
    for (int i = 0; i < bmAllInode_.capacity(); ++i)
    {
        //过滤不关心的inode
        if (!isValidInode(i))
            continue;

        if (bmAllInode_.get(i))
        {           
            if (!(icache_[i].i_flags & EXT4_EXTENTS_FL))
                continue;

            std::vector<ext4_extent> vExtent;//叶子节点

            if (getExtents(icache_[i], vExtent))
            {
                //目录和文件要分别处理
                if (icache_[i].i_mode & S_IFDIR)
                {
                    //顺序遍历extent得到所有数据
                    for (const ext4_extent& extent: vExtent)
                    {
                        //当前extent指向的块数目
                        uint blocks = extent.ee_len;
                        if (blocks > 32768)
                            blocks -= 32768;

                        //一次读出所有数据块
                        vector<byte> buf(fs_.block_size * blocks, 0);
                        read(buf.data(), buf.size(), fs_.block_size * extent.ee_start_lo);

                        //分别读取各个dentry
                        int offset = 0;
                        while (offset < buf.size())
                        {
                            ext4_dir_entry_2 dentry = { 0 };
                            memcpy(&dentry, buf.data() + offset, sizeof ext4_dir_entry_2_head);
                            memcpy(&dentry.name, buf.data() + offset + sizeof ext4_dir_entry_2_head, dentry.name_len);
                            offset += dentry.rec_len;

                            if (strcmp(dentry.name, ".") != 0 &&
                                strcmp(dentry.name, "..") != 0)
                            {
                                //过滤不关心的inode
                                if (isValidInode(dentry.inode))
                                {
                                    //设置inode号的对应路径
                                    pcache_[dentry.inode] = pcache_[i] + dentry.name;
                                    //dentry是目录的话路径后面添加slash
                                    if (dentry.file_type == 0x2)
                                    {
                                        pcache_[dentry.inode] += "/";
                                    }
                                }
                            }
                        }
                    }
                }

                if (icache_[i].i_mode & S_IFREG)
                {
                    uint left = icache_[i].i_size_lo;
                    //顺序遍历extent得到所有数据
                    for (const ext4_extent& extent : vExtent)
                    {
                        //当前extent指向的块数目
                        uint blocks = extent.ee_len;
                        if (blocks > 32768)
                            blocks -= 32768;

                        //一次读出所有数据块
                        vector<byte> buf(fs_.block_size * blocks, 0);
                        read(buf.data(), buf.size(), fs_.block_size * extent.ee_start_lo);
                        //文件大小不一定是块的整数倍，注意计算
                        uint towrite = buf.size();
                        if (buf.size() > left)
                            towrite = left;
                        //添加到文件dcache
                        dcache_[i].insert(dcache_[i].end(), buf.begin(), buf.begin() + towrite);

                        left -= towrite;
                        //应该不用判断left，除非读vExtent出错
                        if (left <= 0)
                            break;
                    }                   
                }
            }
        }
    }

    return true;
}

bool Parser::getExtents(const ext4_inode& inode, std::vector<ext4_extent>& vExtent)
{
    vExtent.clear();

    //单个块或inode.i_block的ext4_extent_idx及ext4_extent_header
    //适用于中间节点
    struct MyExtentIdx
    {
        ext4_extent_header header;
        std::vector<ext4_extent_idx> vidx;
    };

    std::queue<MyExtentIdx> qExtentIdx;//层序遍历extent树

    ext4_extent_header header = { 0 };
    memcpy(&header, inode.i_block, sizeof header);

    //magic相符，且有效项大于0，才继续处理
    if (header.eh_magic != 0xF30A || header.eh_entries == 0)
        return false;

    if (header.eh_depth == 0)//叶子节点
    {
        vExtent.resize(header.eh_entries);

        for (int i = 0; i < header.eh_entries; ++i)
        {
            //直接从i_block拷贝extent
            memcpy(vExtent.data() + i,
                (byte*)inode.i_block + sizeof ext4_extent_header + i * sizeof ext4_extent,
                sizeof ext4_extent);
        }
    }
    else//内部索引节点
    {
        MyExtentIdx myExtentIdx;
        myExtentIdx.header = header;
        myExtentIdx.vidx.assign(header.eh_entries, { 0 });

        for (int i = 0; i < header.eh_entries; ++i)
        {
            //直接从i_block拷贝extent_idx
            memcpy(myExtentIdx.vidx.data() + i,
                   (byte*)inode.i_block + sizeof ext4_extent_header + i * sizeof ext4_extent,
                   sizeof ext4_extent);
        }

        qExtentIdx.push(myExtentIdx);
    }

    //由extent_idx层序遍历得到所有extent
    while (!qExtentIdx.empty())
    {
        MyExtentIdx myEventIdx = qExtentIdx.front();
        qExtentIdx.pop();

        for (const ext4_extent_idx& idx : myEventIdx.vidx)
        {
            vector<byte> buf(fs_.block_size);
            //读取中间节点指向的block
            read(buf.data(), buf.size(), fs_.block_size * idx.ei_block);

            ext4_extent_header header = { 0 };
            memcpy(&header, buf.data(), sizeof header);

            if (header.eh_depth > 0)//索引节点
            {
                MyExtentIdx idx;
                idx.header = header;
                idx.vidx.assign(header.eh_entries, {0});

                for (int i = 0; i < header.eh_entries; ++i)
                {
                    memcpy(&idx.vidx[i],
                           buf.data() + sizeof(header) + i * sizeof ext4_extent_idx,
                           sizeof ext4_extent_idx);
                }

                qExtentIdx.push(idx);//入队列
            }
            else//叶子节点
            {
                vExtent.assign(header.eh_entries, {0});

                for (int i = 0; i < header.eh_entries; ++i)
                {
                    memcpy(vExtent.data() + i,
                           buf.data() + sizeof(header) + i * sizeof ext4_extent,
                           sizeof ext4_extent);
                }
            }
        }
    }

    return true;
}

bool Parser::work()
{
    util::rmdir(dst_);

    //创建所有目录
    for (const std::pair<uint, string>& item : pcache_)
    {
        if (item.second.back() == '/')
        {
            util::mkdir(dst_ + item.second);
        }
    }

    //创建所有文件
    for (const std::pair<uint, vector<byte>>& item : dcache_)
    {
        util::mkfile(dst_ + pcache_[item.first], item.second);
    }

    return true;
}