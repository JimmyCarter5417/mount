#include "parser.h"
#include <cassert>
#include <vector>

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		printf("usage: mount src dst\n");
		exit(1);
	}

	Parser parser(argv[1], argv[2]);

	parser.getFsInfo();
	parser.getGds();
	parser.getInodeBitmap();	
	parser.getInodes();
	parser.getData();
	parser.work();	

	return 0;
}