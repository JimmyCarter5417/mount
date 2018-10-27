#include "parser.h"
#include "define.h"

#include <cassert>
#include <queue>

Parser::Parser(const string& src, const string& dst)
: src_(util::transform(src))
, dst_(util::transform(dst))
, fp_(nullptr)
{
	pcache_[2] = "/";//��Ŀ¼inode�Ź̶�Ϊ2����������ָ��
}

Parser::~Parser()
{
	if (fp_)
	{
		fclose(fp_);
		fp_ = nullptr;
		
	}
}

//����lost+found
bool Parser::isValidInode(uint inode)
{
	return inode == 2 || inode >= 12;
}

//offset��Ϊint���ֻ֧��2G
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
		//flex����ĵ�һ������ĵ�һ���߼��鱣�����superblock������ע��
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
			//��ȡ��i�������inodeλͼ���ڵĿ�
			vector<byte> buf(fs_.block_size);
			int offset = gds_[i].bg_inode_bitmap_lo * fs_.block_size;
			read(buf.data(), buf.size(), offset);
			//�ӿ�����ȡinodeλͼ����
			for (int j = 0; j < fs_.inodes_per_group; ++j)
			{
				if (buf[j >> 3] & (1 << (j & 7)))
					bmAllInode_.set(cur), count++;
				else
					bmAllInode_.clear(cur);

				++cur;
			}
		}
		else
		{
			//δʹ�õĿ���inodeλͼһ������ Ӧ�ò������
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
	//���0��inode������
	for (int i = 0; i < fs_.inode_count; ++i)
	{
		if (bmAllInode_.get(i))
		{
			uint gdIdx = i / fs_.inodes_per_group;//inode���ڵĿ���
			int offset = fs_.inode_size * ((i - 1) % fs_.inodes_per_group)//inode�ڿ���inode����ڲ�ƫ��
				 + gds_[gdIdx].bg_inode_table_lo * fs_.block_size;//����inode��λ��
			
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
		//���˲����ĵ�inode
		if (!isValidInode(i))
			continue;

		if (bmAllInode_.get(i))
		{			
			if (!(icache_[i].i_flags & EXT4_EXTENTS_FL))
				continue;

			std::vector<ext4_extent> vExtent;//Ҷ�ӽڵ�

			if (getExtents(icache_[i], vExtent))
			{
				//Ŀ¼���ļ�Ҫ�ֱ���
				if (icache_[i].i_mode & S_IFDIR)
				{
					//˳�����extent�õ���������
					for (const ext4_extent& extent: vExtent)
					{
						//��ǰextentָ��Ŀ���Ŀ
						uint blocks = extent.ee_len;
						if (blocks > 32768)
							blocks -= 32768;

						//һ�ζ����������ݿ�
						vector<byte> buf(fs_.block_size * blocks, 0);
						read(buf.data(), buf.size(), fs_.block_size * extent.ee_start_lo);

						//�ֱ��ȡ����dentry
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
								//���˲����ĵ�inode
								if (isValidInode(dentry.inode))
								{
									//����inode�ŵĶ�Ӧ·��
									pcache_[dentry.inode] = pcache_[i] + dentry.name;
									//dentry��Ŀ¼�Ļ�·���������slash
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
					//˳�����extent�õ���������
					for (const ext4_extent& extent : vExtent)
					{
						//��ǰextentָ��Ŀ���Ŀ
						uint blocks = extent.ee_len;
						if (blocks > 32768)
							blocks -= 32768;

						//һ�ζ����������ݿ�
						vector<byte> buf(fs_.block_size * blocks, 0);
						read(buf.data(), buf.size(), fs_.block_size * extent.ee_start_lo);
						//�ļ���С��һ���ǿ����������ע�����
						uint towrite = buf.size();
						if (buf.size() > left)
							towrite = left;
						//��ӵ��ļ�dcache
						dcache_[i].insert(dcache_[i].end(), buf.begin(), buf.begin() + towrite);

						left -= towrite;
						//Ӧ�ò����ж�left�����Ƕ�vExtent����
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

	//�������inode.i_block��ext4_extent_idx��ext4_extent_header
	//�������м�ڵ�
	struct MyExtentIdx
	{
		ext4_extent_header header;
		std::vector<ext4_extent_idx> vidx;
	};

	std::queue<MyExtentIdx> qExtentIdx;//�������extent��

	ext4_extent_header header = { 0 };
	memcpy(&header, inode.i_block, sizeof header);

	//magic���������Ч�����0���ż�������
	if (header.eh_magic != 0xF30A || header.eh_entries == 0)
		return false;

	if (header.eh_depth == 0)//Ҷ�ӽڵ�
	{
		vExtent.resize(header.eh_entries);

		for (int i = 0; i < header.eh_entries; ++i)
		{
			//ֱ�Ӵ�i_block����extent
			memcpy(vExtent.data() + i,
				(byte*)inode.i_block + sizeof ext4_extent_header + i * sizeof ext4_extent,
				sizeof ext4_extent);
		}
	}
	else//�ڲ������ڵ�
	{
		MyExtentIdx myExtentIdx;
		myExtentIdx.header = header;
		myExtentIdx.vidx.assign(header.eh_entries, { 0 });

		for (int i = 0; i < header.eh_entries; ++i)
		{
			//ֱ�Ӵ�i_block����extent_idx
			memcpy(myExtentIdx.vidx.data() + i,
				(byte*)inode.i_block + sizeof ext4_extent_header + i * sizeof ext4_extent,
				sizeof ext4_extent);
		}

		qExtentIdx.push(myExtentIdx);
	}

	//��extent_idx��������õ�����extent
	while (!qExtentIdx.empty())
	{
		MyExtentIdx myEventIdx = qExtentIdx.front();
		qExtentIdx.pop();

		for (const ext4_extent_idx& idx : myEventIdx.vidx)
		{
			vector<byte> buf(fs_.block_size);
			//��ȡ�м�ڵ�ָ���block
			read(buf.data(), buf.size(), fs_.block_size * idx.ei_block);

			ext4_extent_header header = { 0 };
			memcpy(&header, buf.data(), sizeof header);

			if (header.eh_depth > 0)//�����ڵ�
			{
				MyExtentIdx idx;
				idx.header = header;
				idx.vidx.assign(header.eh_entries, {0});

				for (int i = 0; i < header.eh_entries; ++i)
				{
					memcpy(&idx.vidx[i],
						buf.data() + sizeof(header)+i * sizeof ext4_extent_idx,
						sizeof ext4_extent_idx);
				}

				qExtentIdx.push(idx);//�����
			}
			else//Ҷ�ӽڵ�
			{
				vExtent.assign(header.eh_entries, {0});

				for (int i = 0; i < header.eh_entries; ++i)
				{
					memcpy(vExtent.data() + i,
						buf.data() + sizeof(header)+i * sizeof ext4_extent,
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

	//��������Ŀ¼
	for (const std::pair<uint, string>& item : pcache_)
	{
		if (item.second.back() == '/')
		{
			util::mkdir(dst_ + item.second);
		}
	}

	//���������ļ�
	for (const std::pair<uint, vector<byte>>& item : dcache_)
	{
		util::mkfile(dst_ + pcache_[item.first], item.second);
	}

	return true;
}