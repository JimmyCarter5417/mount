#ifndef PARSER_H
#define PARSER_H

#include "define.h"
#include "util.h"

#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

using std::string;

class Parser
{
public:
	Parser(const string& src, const string& dst);
	~Parser();
	//disable copy
	Parser(const Parser&) = delete;
	Parser& operator = (Parser&) = delete;
	
	struct FsInfo
	{		
		uint block_count;//s_blocks_count_lo
		uint block_size;//1024 * 2 ^ s_log_block_size
		uint inode_count;//s_inodes_count
		uint inode_size;//s_inode_size
		uint blocks_per_group;//s_blocks_per_group
		uint inodes_per_group;//s_inodes_per_group
		uint groups_per_flex;//2 ^ s_log_groups_per_flex
		
		uint group_count;//block_count / blocks_per_group
		uint flex_count;//block_count / blocks_per_group / groups_per_flex
	};

	bool read(void* buf, uint len, uint offset = 0);

	bool isValidInode(uint inode);

	bool getFsInfo();
	bool getGds();
	bool getInodeBitmap();
	bool getInodes();
	bool getData();

	bool getExtents(const ext4_inode& inode, std::vector<ext4_extent>& vExtent);

	bool work();
	
private:
	string src_;//file path
	string dst_;//dir path
	FILE* fp_;	

	FsInfo fs_;

	util::Bitmap bmAllInode_;

	std::vector<ext4_group_desc> gds_;//0, 1, 2, ... , fs_.group_count - 1

	std::unordered_map<uint, ext4_inode> icache_;//inode num -> inode
	std::unordered_map<uint, string> pcache_;//inode num -> path
	std::unordered_map<uint, vector<byte>> dcache_;//inode num -> data，仅限普通文件
};

#endif//PARSER_H