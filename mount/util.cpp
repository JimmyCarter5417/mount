#include "util.h"

#include <io.h>

string util::transform(const string& path)
{
	string path1 = path;

	std::transform(path1.begin(), path1.end(), path1.begin(),
		            [](char ch)
	                {
		                return ch == '/' ? '\\' : ch;
	                });

	return path1;
}

bool util::mkdir(const string& path)
{
	//将斜杠转为反斜杠
	string path1 = util::transform(path);

	if (_access(path1.c_str(), 06) == 0)
		return true;

	char cmd[512] = {0};
	sprintf(cmd, "mkdir %s", path1.c_str());//必须用反斜杠分隔
	system(cmd);

	return _access(path1.c_str(), 06) == 0;//读写
}

bool util::mkfile(const string& path, const vector<byte>& buf)
{
	//将斜杠转为反斜杠
	string path1 = util::transform(path);

	FILE* fp = fopen(path1.c_str(), "wb+");
	if (!fp)
	{
		rmfile(path1);
		return false;
	}

	size_t ret = fwrite(buf.data(), 1, buf.size(), fp);
	if (ret != buf.size())
	{
		rmfile(path1);
		return false;
	}
		
	return true;
}

bool util::rmdir(const string& path)
{
	//将斜杠转为反斜杠
	string path1 = util::transform(path);

	if (_access(path1.c_str(), 0) == -1)
		return true;

	char cmd[512] = { 0 };
	sprintf(cmd, "rd /s /q %s", path1.c_str());//必须用反斜杠分隔
	system(cmd);

	return _access(path1.c_str(), 0) == -1;//不存在
}

bool util::rmfile(const string& path)
{
	//将斜杠转为反斜杠
	string path1 = util::transform(path);

	if (_access(path1.c_str(), 0) == -1)
		return true;

	char cmd[512] = { 0 };
	sprintf(cmd, "del /f /s /q %s", path1.c_str());//必须用反斜杠分隔
	system(cmd);

	return _access(path1.c_str(), 0) == -1;//不存在
}