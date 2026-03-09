#include "parser.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace shell {

namespace {

constexpr int OK = 0;
constexpr int FAIL = 1;
constexpr int BAD = -1;

struct State {
	int code = 0;
	bool need_exit = false;
};

struct Chain {
	std::vector<command> cmds;
};

// закрыть дескриптор
void close_fd(int &fd) {
	if (fd != BAD) {
		close(fd);
		fd = BAD;
	}
}

// открыть файл для вывода
	int open_output(output_type type, const std::string &path) {
	int flags = O_WRONLY | O_CREAT | O_CLOEXEC;

	if (type == OUTPUT_TYPE_FILE_NEW) {
		flags |= O_TRUNC;
	} else if (type == OUTPUT_TYPE_FILE_APPEND) {
		flags |= O_APPEND;
	} else {
		return BAD;
	}

	int fd = open(path.c_str(), flags, 0666);
	if (fd == BAD)
		std::cerr << "bash: " << path << ": " << strerror(errno) << "\n";

	return fd;
}

// перевести wait-статус
int bash_code(int status) {
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return FAIL;
}

// дождаться процесса
int wait_one(pid_t pid) {
	int status = 0;

	while (true) {
		pid_t rc = waitpid(pid, &status, 0);
		if (rc == BAD && errno == EINTR) {
			errno = 0;
			continue;
		}
		if (rc == BAD)
			return FAIL;
		break;
	}

	return bash_code(status);
}

// собрать argv
std::vector<char *> make_argv(command &cmd) {
	std::vector<char *> argv;
	argv.reserve(cmd.args.size() + 2);
	argv.push_back(cmd.exe.data());
	for (std::string &arg : cmd.args)
		argv.push_back(arg.data());
	argv.push_back(nullptr);
	return argv;
}

// выполнить внешнюю команду
[[noreturn]] void run_exec(command &cmd) {
	auto argv = make_argv(cmd);
	execvp(argv[0], argv.data());

	if (errno == ENOENT) {
		std::cerr << "bash: " << argv[0] << ": command not found\n";
		_exit(127);
	}

	std::cerr << "bash: " << argv[0] << ": " << strerror(errno) << "\n";
	_exit(126);
}

// builtin cd
	int run_cd(const std::vector<std::string> &args, output_type type, const std::string &path) {
	// создать файл при редиректе
	if (type != OUTPUT_TYPE_STDOUT) {
		int out_fd = open_output(type, path);
		if (out_fd == BAD)
			return FAIL;
		close_fd(out_fd);
	}

	if (args.empty()) {
		const char *home = getenv("HOME");
		if (home == nullptr || home[0] == '\0') {
			std::cerr << "bash: cd: HOME not set\n";
			return FAIL;
		}
		if (chdir(home) == 0)
			return OK;
		std::cerr << "bash: cd: " << home << ": " << strerror(errno) << "\n";
		return FAIL;
	}

	if (args.size() > 1) {
		std::cerr << "bash: cd: too many arguments\n";
		return FAIL;
	}

	if (chdir(args[0].c_str()) == 0)
		return OK;

	std::cerr << "bash: cd: " << args[0] << ": " << strerror(errno) << "\n";
	return FAIL;
}

// builtin exit в shell
State run_exit_parent(const std::vector<std::string> &args) {
	if (args.empty())
		return {0, true};

	errno = 0;
	char *end = nullptr;
	long value = std::strtol(args[0].c_str(), &end, 10);

	if (errno != 0 || end == args[0].c_str() || *end != '\0') {
		std::cerr << "bash: exit: " << args[0] << ": numeric argument required\n";
		return {2, true};
	}

	if (args.size() > 1) {
		std::cerr << "bash: exit: too many arguments\n";
		return {1, false};
	}

	return {static_cast<int>(static_cast<unsigned char>(value)), true};
}

// builtin exit в child
int run_exit_child(const std::vector<std::string> &args) {
	if (args.empty())
		return 0;

	errno = 0;
	char *end = nullptr;
	long value = std::strtol(args[0].c_str(), &end, 10);

	if (errno != 0 || end == args[0].c_str() || *end != '\0') {
		std::cerr << "bash: exit: " << args[0] << ": numeric argument required\n";
		return 2;
	}

	if (args.size() > 1) {
		std::cerr << "bash: exit: too many arguments\n";
		return 1;
	}

	return static_cast<int>(static_cast<unsigned char>(value));
}

// builtin в родителе
State try_parent_builtin(const command &cmd, output_type type, const std::string &path) {
	if (cmd.exe == "cd")
		return {run_cd(cmd.args, type, path), false};

	if (cmd.exe != "exit")
		return {-1, false};

	if (type == OUTPUT_TYPE_STDOUT)
		return run_exit_parent(cmd.args);

	int fd = open_output(type, path);
	if (fd == BAD)
		return {1, false};
	close_fd(fd);

	State st = run_exit_parent(cmd.args);
	st.need_exit = false;
	return st;
}

// builtin в потомке
int try_child_builtin(const command &cmd) {
	if (cmd.exe == "cd")
		return run_cd(cmd.args, OUTPUT_TYPE_STDOUT, "");
	if (cmd.exe == "exit")
		return run_exit_child(cmd.args);
	return -1;
}

// собрать фоновые процессы
void reap_background() {
	while (true) {
		int status = 0;
		pid_t rc = waitpid(-1, &status, WNOHANG);

		if (rc == BAD && errno == EINTR) {
			errno = 0;
			continue;
		}
		if (rc <= 0)
			break;
	}
}

// выполнить одну команду
State run_single(command &cmd, output_type type, const std::string &path) {
	State st = try_parent_builtin(cmd, type, path);
	if (st.code >= 0)
		return st;

	int out_fd = BAD;
	if (type != OUTPUT_TYPE_STDOUT) {
		out_fd = open_output(type, path);
		if (out_fd == BAD)
			return {1, false};
	}

	pid_t pid = fork();
	if (pid == BAD) {
		std::cerr << "bash: fork: " << strerror(errno) << "\n";
		close_fd(out_fd);
		return {1, false};
	}

	if (pid == 0) {
		if (type != OUTPUT_TYPE_STDOUT) {
			if (dup2(out_fd, STDOUT_FILENO) == BAD) {
				std::cerr << "bash: dup2: " << strerror(errno) << "\n";
				close_fd(out_fd);
				_exit(1);
			}
			close_fd(out_fd);
		}

		run_exec(cmd);
	}

	close_fd(out_fd);
	return {wait_one(pid), false};
}

// выполнить pipeline
int run_pipeline(std::vector<command> &cmds, output_type type, const std::string &path) {
	if (cmds.empty())
		return 0;

	std::vector<pid_t> pids;
	int prev_in = BAD;
	int out_fd = BAD;

	for (size_t i = 0; i < cmds.size(); ++i) {
		bool last = (i + 1 == cmds.size());
		int pipe_fd[2] = {BAD, BAD};

		if (!last) {
			if (pipe(pipe_fd) == BAD) {
				std::cerr << "bash: pipe: " << strerror(errno) << "\n";
				close_fd(prev_in);
				for (pid_t pid : pids)
					(void)wait_one(pid);
				return 1;
			}
		} else if (type != OUTPUT_TYPE_STDOUT) {
			out_fd = open_output(type, path);
			if (out_fd == BAD) {
				close_fd(prev_in);
				for (pid_t pid : pids)
					(void)wait_one(pid);
				return 1;
			}
		}

		pid_t pid = fork();
		if (pid == BAD) {
			std::cerr << "bash: fork: " << strerror(errno) << "\n";
			close_fd(prev_in);
			close_fd(pipe_fd[0]);
			close_fd(pipe_fd[1]);
			close_fd(out_fd);
			for (pid_t child : pids)
				(void)wait_one(child);
			return 1;
		}

		if (pid == 0) {
			// подключить вход
			if (prev_in != BAD && dup2(prev_in, STDIN_FILENO) == BAD) {
				std::cerr << "bash: dup2: " << strerror(errno) << "\n";
				_exit(1);
			}

			// подключить выход в pipe
			if (!last && dup2(pipe_fd[1], STDOUT_FILENO) == BAD) {
				std::cerr << "bash: dup2: " << strerror(errno) << "\n";
				_exit(1);
			}

			// подключить редирект
			if (last && type != OUTPUT_TYPE_STDOUT && dup2(out_fd, STDOUT_FILENO) == BAD) {
				std::cerr << "bash: dup2: " << strerror(errno) << "\n";
				_exit(1);
			}

			close_fd(prev_in);
			close_fd(pipe_fd[0]);
			close_fd(pipe_fd[1]);
			close_fd(out_fd);

			int builtin_code = try_child_builtin(cmds[i]);
			if (builtin_code >= 0)
				_exit(builtin_code);

			run_exec(cmds[i]);
		}

		pids.push_back(pid);
		close_fd(prev_in);
		close_fd(out_fd);
		close_fd(pipe_fd[1]);
		prev_in = last ? BAD : pipe_fd[0];
	}

	close_fd(prev_in);

	int last_code = 1;
	for (size_t i = 0; i < pids.size(); ++i) {
		int cur = wait_one(pids[i]);
		if (i + 1 == pids.size())
			last_code = cur;
	}

	return last_code;
}

// разбить строку на цепочки
bool split_exprs(const command_line *line, std::vector<Chain> &chains, std::vector<expr_type> &ops) {
	Chain cur;

	for (const expr &e : line->exprs) {
		if (e.type == EXPR_TYPE_COMMAND) {
			cur.cmds.push_back(*e.cmd);
			continue;
		}

		if (e.type == EXPR_TYPE_PIPE)
			continue;

		if (e.type != EXPR_TYPE_AND && e.type != EXPR_TYPE_OR)
			return false;

		if (cur.cmds.empty())
			return false;

		chains.push_back(std::move(cur));
		cur = Chain();
		ops.push_back(e.type);
	}

	if (!cur.cmds.empty())
		chains.push_back(std::move(cur));

	return !chains.empty() && chains.size() == ops.size() + 1;
}

// выполнить одну цепочку
State run_chain(Chain &chain, output_type type, const std::string &path) {
	if (chain.cmds.size() == 1)
		return run_single(chain.cmds[0], type, path);
	return {run_pipeline(chain.cmds, type, path), false};
}

// выполнить строку в foreground
State run_foreground(const command_line *line, State state) {
	std::vector<Chain> chains;
	std::vector<expr_type> ops;

	if (!split_exprs(line, chains, ops))
		return {1, false};

	for (size_t i = 0; i < chains.size(); ++i) {
		bool need_run = false;

		if (i == 0)
			need_run = true;
		else if (ops[i - 1] == EXPR_TYPE_AND)
			need_run = (state.code == 0);
		else if (ops[i - 1] == EXPR_TYPE_OR)
			need_run = (state.code != 0);

		if (!need_run)
			continue;

		bool last = (i + 1 == chains.size());
		output_type out_type = last ? line->out_type : OUTPUT_TYPE_STDOUT;
		const std::string &out_file = last ? line->out_file : std::string();

		state = run_chain(chains[i], out_type, out_file);
		if (state.need_exit)
			return state;
	}

	return state;
}

// выполнить строку
State run_line(const command_line *line, State state) {
	if (!line->is_background)
		return run_foreground(line, state);

	pid_t pid = fork();
	if (pid == BAD) {
		std::cerr << "bash: fork: " << strerror(errno) << "\n";
		return {1, false};
	}

	if (pid == 0) {
		State child_state = run_foreground(line, state);
		_exit(child_state.code);
	}

	return {0, false};
}

// забрать готовые строки из parser
State consume_lines(parser *p, State state) {
	while (!state.need_exit) {
		command_line *raw = nullptr;
		parser_error err = parser_pop_next(p, &raw);

		if (err == PARSER_ERR_NONE && raw == nullptr)
			break;

		std::unique_ptr<command_line> holder(raw);

		if (err != PARSER_ERR_NONE)
			continue;

		reap_background();
		state = run_line(raw, state);
	}

	return state;
}

} // namespace

} // namespace shell

int main() {
	parser *p = parser_new();
	shell::State state = {0, false};

	constexpr size_t kBufSize = 16 * 1024;
	char buf[kBufSize];

	while (!state.need_exit) {
		shell::reap_background();

		ssize_t rc = read(STDIN_FILENO, buf, kBufSize);
		if (rc == 0)
			break;

		if (rc < 0) {
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			state.code = 1;
			break;
		}

		parser_feed(p, buf, static_cast<uint32_t>(rc));
		state = shell::consume_lines(p, state);
	}

	state = shell::consume_lines(p, state);
	shell::reap_background();
	parser_delete(p);
	return state.code;
}