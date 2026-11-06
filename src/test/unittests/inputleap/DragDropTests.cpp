#include <gtest/gtest.h>
#include "inputleap/DragInformation.h"
#include "inputleap/DropHelper.h"
#include "io/filesystem.h"

#include <fstream>
#include <string>

using namespace inputleap;

TEST(DragDropTests, parseDragInfo_singleUnixFile_correctlyParsed)
{
    DragFileList fileList;
    std::string data = "/home/user/test.txt,1024,";
    DragInformation::parseDragInfo(fileList, 1, data);

    ASSERT_EQ(1u, fileList.size());
    EXPECT_EQ("test.txt", fileList.at(0).getFilename());
    EXPECT_EQ(1024u, fileList.at(0).getFilesize());
}

TEST(DragDropTests, parseDragInfo_singleWindowsFile_correctlyParsed)
{
    DragFileList fileList;
    std::string data = "C:\\Users\\User\\Documents\\image.png,4096,";
    DragInformation::parseDragInfo(fileList, 1, data);

    ASSERT_EQ(1u, fileList.size());
    EXPECT_EQ("image.png", fileList.at(0).getFilename());
    EXPECT_EQ(4096u, fileList.at(0).getFilesize());
}

TEST(DragDropTests, parseDragInfo_multipleFiles_allParsedCorrectly)
{
    DragFileList fileList;
    std::string data = "/tmp/first.txt,100,/tmp/second.log,200,";
    DragInformation::parseDragInfo(fileList, 2, data);

    ASSERT_EQ(2u, fileList.size());
    EXPECT_EQ("first.txt", fileList.at(0).getFilename());
    EXPECT_EQ(100u, fileList.at(0).getFilesize());
    EXPECT_EQ("second.log", fileList.at(1).getFilename());
    EXPECT_EQ(200u, fileList.at(1).getFilesize());
}

TEST(DragDropTests, getDragFileExtension_validExtensions)
{
    EXPECT_EQ("txt", DragInformation::getDragFileExtension("test.txt"));
    EXPECT_EQ("png", DragInformation::getDragFileExtension("photo.png"));
    EXPECT_EQ("", DragInformation::getDragFileExtension("noextension"));
}

TEST(DragDropTests, dropHelper_writeToDir_createsFileWithContent)
{
    std::string testDir = "/tmp/inputleap_test_drop";
    DragFileList fileList;
    DragInformation di;
    di.setFilename("drop_test_file.txt");
    di.setFilesize(11);
    fileList.push_back(di);

    std::string fileContent = "hello world";
    DropHelper::writeToDir(testDir, fileList, fileContent);

    // Verify file exists and content matches
    std::string targetFile = testDir + "/drop_test_file.txt";
    std::ifstream in(targetFile, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    std::string readContent((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    EXPECT_EQ(fileContent, readContent);
    EXPECT_TRUE(fileList.empty());

    // Clean up
    std::remove(targetFile.c_str());
    std::error_code ec;
    inputleap::fs::remove(inputleap::fs::u8path(testDir), ec);
}
