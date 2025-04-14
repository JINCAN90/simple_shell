#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>   //unistd.h 包含了许多系统调用函数，像 fork、execvp 等
#include <sys/wait.h> //sys/wait.h 用于进程等待操作
#include <fcntl.h>  //fcntl.h 用于文件控制操作
#include <signal.h> //signal.h 用于信号处理
#include <map> //map 是关联容器，用于存储键值对

using namespace std; //使用 std 命名空间，这样就不用在每次使用标准库中的类和函数时都加上                                    std:: 前缀


//定义了一个名为 BackgroundProcess 的结构体，用于存储后台进程的相关信息
// 后台进程信息结构体
struct BackgroundProcess {
    pid_t pid;   //进程的 ID
    string command;  //进程执行的命令
    bool is_running;  //进程是否正在运行
};

map<pid_t, BackgroundProcess> bg_processes; // 创建一个 map 容器 bg_processes，键                                                              为进程 ID，值为 BackgroundProcess                                                              结构体对象，用来存储所有后台进程的信息

// 信号处理函数（回收僵尸进程），用于处理 SIGCHLD 信号（当子进程终止或停止时会发送该信号）
void sigchld_handler(int sig) {
    pid_t pid;
    int status;
    // WNOHANG表示非阻塞等待
    //waitpid(-1, &status, WNOHANG) 以非阻塞方式等待任意子进程结束，若有子进程结束，返回其进程 ID。若       该进程 ID 存在于 bg_processes 中，将其 is_running 标志设为 false并输出进程退出信息
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (bg_processes.count(pid)) {
            bg_processes[pid].is_running = false;
            cout << "\n[" << pid << "] exited" << endl;
        }
    }
}

// 解析输入命令
//将输入的字符串按空格或制表符分割成多个单词，存储在 vector<string> 中并返回
vector<string> tokenize(const string& input) {
    vector<string> tokens;
    string token;
    for (char c : input) {
        if (c == ' ' || c == '\t') {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        }
        else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

// 执行单个命令
//execute_command 函数用于执行单个命令。若命令为空则直接返回
//对于内置命令 cd，若参数不足则输出错误信息，若 chdir 调用失败则输出错误信息；对于 jobs 命令，遍历 bg_processes 并输出每个后台进程的信息。若不是内置命令，使用 fork 创建子进程，子进程将参数转换为 execvp 所需的格式并执行命令，若执行失败则输出错误信息并退出；父进程根据 background 标志决定是等待子进程结束还是将其添加到后台进程列表。
void execute_command(vector<string> args, bool background = false) {
    if (args.empty()) return;

    // 处理内置命令
    if (args[0] == "cd") {
        if (args.size() < 2) {
            cerr << "cd: missing argument" << endl;
        }
        else if (chdir(args[1].c_str()) != 0) {
            perror("cd");
        }
        return;
    }

    if (args[0] == "jobs") {
        for (auto& [pid, proc] : bg_processes) {
            cout << "[" << pid << "] " << proc.command
                << (proc.is_running ? " (running)" : " (stopped)") << endl;
        }
        return;
    }

    // 创建子进程
    pid_t pid = fork();
    if (pid == 0) { // 子进程
        // 转换参数格式
        char** argv = new char* [args.size() + 1];
        for (size_t i = 0; i < args.size(); ++i) {
            argv[i] = const_cast<char*>(args[i].c_str());
        }
        argv[args.size()] = nullptr;

        execvp(argv[0], argv);
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    else if (pid > 0) { // 父进程
        if (!background) {
            // 前台等待
            waitpid(pid, nullptr, 0);
        }
        else {
            // 添加到后台进程列表
            bg_processes[pid] = { pid, args[0], true };
            cout << "[" << pid << "] " << args[0] << endl;
        }
    }
    else {
        perror("fork");
    }
}

// 处理重定向
//handle_redirection 函数用于处理输出重定向。遍历命令参数，若遇到 > 符号，检查其后是否有文件名，若没有则   输出错误信息；若有则打开文件，使用 dup2 将标准输出重定向到该文件，关闭文件描述符，从参数列表中移除 > 和   文件名
void handle_redirection(vector<string>& args) {
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == ">") {
            if (next(it) == args.end()) {
                cerr << "Syntax error: no output file" << endl;
                return;
            }
            string filename = *(next(it));
            int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
                perror("open");
                return;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            args.erase(it, it + 2);
            break;
        }
    }
}

// 处理管道
//execute_pipeline 函数用于处理管道命令。通过循环依次处理每个命令，使用 pipe 创建管道，fork 创建子进     程。子进程根据位置进行输入输出重定向，然后执行命令；父进程关闭不需要的文件描述符，更新 prev_pipe_read。   最后等待所有子进程结束
void execute_pipeline(vector<vector<string>> commands) {
    int prev_pipe_read = -1;

    for (size_t i = 0; i < commands.size(); ++i) {
        int pipefd[2];
        if (i < commands.size() - 1) {
            if (pipe(pipefd) == -1) {
                perror("pipe");
                return;
            }
        }

        pid_t pid = fork();
        if (pid == 0) { // 子进程
            // 输入重定向
            if (prev_pipe_read != -1) {
                dup2(prev_pipe_read, STDIN_FILENO);
                close(prev_pipe_read);
            }

            // 输出重定向
            if (i < commands.size() - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }

            // 执行命令
            execute_command(commands[i]);
            exit(EXIT_SUCCESS);
        }
        else if (pid > 0) { // 父进程
            if (prev_pipe_read != -1) close(prev_pipe_read);
            if (i < commands.size() - 1) {
                close(pipefd[1]);
                prev_pipe_read = pipefd[0];
            }
        }
        else {
            perror("fork");
            return;
        }
    }

    // 等待最后一个命令
    if (prev_pipe_read != -1) close(prev_pipe_read);
    while (wait(nullptr) > 0);
}

//首先注册 SIGCHLD 信号的处理函数。然后进入一个无限循环，不断输出提示符 mysh> 并读取用户输入。若输入包含   管道符号 |，则将输入分割成多个命令并调用 execute_pipeline 处理；否则，将输入分割成参数，检查是否为后     台执行，处理输出重定向，最后调用 execute_command 执行命令
int main() {
    // 注册信号处理
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, nullptr) == -1) {
        perror("sigaction");
        return 1;
    }

    while (true) {
        cout << "mysh> ";
        cout.flush();

        string input;
        if (!getline(cin, input)) break;

        // 处理管道
        size_t pipe_pos = input.find('|');
        if (pipe_pos != string::npos) {
            vector<vector<string>> commands;
            size_t start = 0;
            while ((pipe_pos = input.find('|', start)) != string::npos) {
                string cmd = input.substr(start, pipe_pos - start);
                commands.push_back(tokenize(cmd));
                start = pipe_pos + 1;
                if (pipe_pos == string::npos) break;
            }
            commands.push_back(tokenize(input.substr(start)));
            execute_pipeline(commands);
            continue;
        }

        // 处理普通命令
        vector<string> args = tokenize(input);
        if (args.empty()) continue;

        // 检查后台执行
        bool background = false;
        if (args.back() == "&") {
            background = true;
            args.pop_back();
        }

        // 处理输出重定向
        handle_redirection(args);

        execute_command(args, background);
    }

    return 0;
}