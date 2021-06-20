#include <glog/logging.h>
#include <sys/sendfile.h>
#include "Session.h"
#include "../util/StringUtil.h"
#include "Server.h"
#include "../client/Client.h"

#define PATH_MAX 4096

static std::string shareRoot = "share";

void Session::then(SessionStatus pStatus) {
    status = pStatus;
}

void Session::handle(const EventContext &context) {
    if (status == READING) {
        // 允许请求包的读入并回复短包
        // 读入长度
        connSocket->read(*readBuffer, 4);
        uint32_t packetLen = readBuffer->readUInt(0);
        // 读入包
        ssize_t readLen = connSocket->read(*readBuffer, packetLen);
        if (readLen <= 0) {
            if (readLen == -1)
                then(END);
            return;
        }
        LOG(INFO) << "Read request packet plen = " << packetLen << ", rlen = " << readLen;
        // 分别处理
        Byte type = (*readBuffer)[0];
        switch (type) {
            case COMMAND:
                // 处理指令请求
                processCommand(readLen);
                return;
            case DOWNLOAD:
                // 处理下载请求
                processDownload(readLen, false);
                return;
            case DOWNLOAD_INFO:
                // 处理下载请求
                processDownload(readLen, true);
                return;
        }
        // 不符合协议直接关闭
        then(END);
        return;
    } else if (status == WRITING) {
        // 文件写出
        handleWriting();
    }
}

SessionStatus Session::getStatus() const {
    return status;
}

Session::Session(std::shared_ptr<Socket> connSocket) : connSocket(std::move(connSocket)) {
}

void Session::processCommand(uint32_t readLen) {
    // 读入指令
    auto bufCmd = readBuffer->slice(1, readLen);
    bufCmd.write((uint16_t) 0);
    std::string cmd(reinterpret_cast<char *>(bufCmd.data()));
    auto cmds = StringUtil::split(cmd, " ");
    // 解析指令
    LOG(INFO) << "Read command: " << cmd;
    // 远端指令、协作指令的远端部分
    if (cmd == "quit" || cmd == "q") {
        // 关闭会话
        say("Bye\n");
        then(END);
        return;
    } else if (cmds[0] == "ls" || cmds[0] == "list") {
        // 列出目录内容
        // 解析参数
        if (cmds.size() != 2) {
            say("list 指令需要 1 个参数!\n");
            return;
        }
        auto path = validatePath(cmds[1]);
        if (path.empty()) {
            return;
        }
        // 运行 ls 指令
        auto pFile = File::popen("ls -al '" + path + "'", "r");
        auto cmdLen = pFile->read(*readBuffer);
        if (cmdLen == 0) {
            // 目录不存在
            say("目录不存在!\n");
            return;
        }
        ByteBuffer resultBuf = readBuffer->slice(0, cmdLen);
        sendResponse(PLAIN_TEXT, resultBuf);
        return;
    } else if (cmd == "shutdown") {
        // 关闭服务器
        say("关闭服务器...\n");
        Server::INSTANCE->kill();
        return;
    } else if (cmds[0] == "link") {
        // 要求连接客户端
        if (Client::INSTANCE) {
            say("已经连接其他客户端！");
            return;
        }
        // 创建客户端
        Client::INSTANCE = std::make_unique<Client>();
        auto &client = *Client::INSTANCE;
        // 设置参数
        client.srvAddress = cmds[1];
        client.srvPort = std::stoi(cmds[2]);
        // 初始化客户端
        client.init();
        client.connect();
        // 启动客户端
        Server::INSTANCE->clientAttached = true;
        say("连接成功");
        std::cout << "成功与 " << cmds[1] << " 建立连接！输入 start 开始聊天" << std::endl;
        return;
    } else if (cmds[0] == "say") {
        // 发送消息
        auto msg = cmd.substr(4, cmd.size() - 4);
        say("[我] " + msg + "\n");
        std::cout << "[对方] " << msg << std::endl;
        return;
    } else if (cmds[0] == "send") {
        // 请求发送文件，客户端本地保存文件名
        auto &file = cmds[1];
        say("正在请求传输文件...\n");
        std::cout << "对方请求传输文件 " << file << "，使用 confirm <保存路径> 同意，使用 cancel 拒绝" << std::endl;
        Client::INSTANCE->fileToDownload = file;
        return;
    } else if (cmds[0] == "confirm") {
        // 同意对方发送文件，对方主动发起下载（Client负责），本方不作操作
        std::cout << "对方同意传输文件，请等待文件传输完成！" << std::endl;
        say("开始传输文件...\n");
        return;
    } else if (cmd == "cancel") {
        // 拒绝对方发送文件
        std::cout << "对方拒绝传输文件！" << std::endl;
        say("已拒绝传输文件\n");
        return;
    } else if (cmd == "finish") {
        std::cout << "传输完成！" << std::endl;
        return;
    }
    // 否则其余指令 Echo
    say("未知命令: " + cmd + "\n");
}

void Session::processDownload(uint32_t readLen, bool infoOnly) {
    // 防止缓冲区溢出
    if (readLen < 1 + sizeof(DownloadRequestPacket))
        return;
    // 读入结构体
    DownloadRequestPacket req = *reinterpret_cast<DownloadRequestPacket *>(readBuffer->data() + 1);
    // 读入文件路径
    auto bufFile = readBuffer->slice(1 + sizeof(DownloadRequestPacket), readLen);
    bufFile.write((uint16_t) 0);
    std::string filepath(reinterpret_cast<char *>(bufFile.data()));
    // 处理
    LOG(INFO) << "Req download file = '" << filepath << "', offset = " << req.offset << ", size = " << req.size;
    // 打开文件
    auto file = openDownloadFile(filepath);
    if (!file) {
        return;
    }
    // 校验大小
    auto fileSize = file->getSize();
    if (req.size == 0) {
        req.size = fileSize;
    }
    if (!(req.offset <= fileSize && req.size <= fileSize && (req.offset + req.size) <= fileSize)) {
        say("Bad file size\n");
        return;
    }
    // 发送下载头部
    DownloadInfoPacket resp{
        .size = req.size
    };
    sendResponse(infoOnly ? DOWNLOAD_INFO_REPLY : BINARY, ByteBuffer(&resp, sizeof(DownloadInfoPacket)));
    // 设置下载状态
    if (!infoOnly) {
        fileProcess = std::make_unique<FileProcess>(file, req.size);
        file->seek(req.offset);
        then(WRITING);
    }
}

void Session::say(const std::string &str) const {
    LOG(INFO) << "Reply response: " << str;
    sendResponse(PLAIN_TEXT, ByteBuffer::str(str));
}

bool Session::sendResponse(ResponseType type, const ByteBuffer &buffer) const {
    ByteBuffer sendBuffer;
    // Write len
    uint32_t len = 1 + buffer.size();
    sendBuffer.write(len);
    // Write type
    sendBuffer.push_back(type);
    // Write resp
    sendBuffer = sendBuffer + buffer;
    size_t n = connSocket->write(reinterpret_cast<const char *>(sendBuffer.data()), sendBuffer.size());
    // check
    if (n != sendBuffer.size()) {
        LOG(WARNING) << "Cannot send response, resp = " << sendBuffer.size() << ", n = " << n;
        return false;
    }
    return true;
}

std::shared_ptr<File> Session::openDownloadFile(const std::string &filepath) const {
    auto realPath = validatePath(filepath);
    if (realPath.empty()) {
        return nullptr;
    }

    // 打开文件
    auto file = File::open(realPath, "rb");
    LOG(INFO) << "Open file: " << realPath;
    if (!file) {
        if (errno == ENOENT) {
            // 文件不存在
            say("File not exist\n");
        } else {
            say("Unexpected file error\n");
        }
        PLOG(INFO) << "Cannot open file";
        return nullptr;
    }
    return file;
}

void Session::handleWriting() {
    if (!fileProcess) {
        say("Server internal error");
        LOG(ERROR) << "No file is sending";
        then(END);
    }

    // 继续传输
    int fileFd = fileProcess->file->getFd();
    size_t rest = fileProcess->rest;
    ssize_t transferred;
    while (rest > 0) {
        // 零拷贝传输
        transferred = sendfile(connSocket->fd(), fileFd, nullptr, rest);
        if (transferred < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 结束一次传输
                break;
            } else {
                // 传输发生异常
                PLOG(ERROR) << "Error sending file to " << connSocket->getAddress() << ":" << connSocket->getPort();
                then(END);
                return;
            }
        } else if (transferred == 0) {
            // 连接关闭
            then(END);
            return;
        } else {
            rest -= transferred;
        }
    }
    // 结束一段传输
    if (rest == 0) {
        LOG(INFO) << "File transferred";
        then(READING);
    } else {
        // 传输还未结束
        fileProcess->rest = rest;
    }
}

std::string Session::validatePath(const std::string& path) const {
    char realPath[PATH_MAX];
    char curPath[PATH_MAX];

    // 过滤危险字符
    if (strpbrk(path.c_str(), "<|>&'")) {
        say("Bad file path\n");
        LOG(INFO) << "Malicious file path (bad char): " << path;
        return "";
    }
    // 转绝对地址
    realpath((shareRoot + "/" + path).c_str(), realPath);
    // 地址校验
    getcwd(curPath, PATH_MAX);
    strcat(curPath, ("/" + shareRoot).c_str());
    if (strncmp(realPath, curPath, strlen(curPath)) != 0) {
        say("Bad file path\n");
        LOG(INFO) << "Malicious file path (out of dir): " << path;
        return "";
    }

    return realPath;
}

void Session::attachBuffer(std::shared_ptr<ByteBuffer> buf) {
    readBuffer = std::move(buf);
}

void Session::detachBuffer() {
    readBuffer = nullptr;
}
