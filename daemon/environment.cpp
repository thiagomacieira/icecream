/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <config.h>
#include "environment.h"
#include <logging.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#ifdef HAVE_SCHED_H
#include <sched.h>
#endif

#include "comm.h"
#include "exitcode.h"
#include "util.h"

using namespace std;

size_t sumup_dir(const string &dir)
{
    size_t res = 0;
    DIR *envdir = opendir(dir.c_str());

    if (!envdir) {
        return res;
    }

    struct stat st;

    string tdir = dir + "/";

    for (struct dirent *ent = readdir(envdir); ent; ent = readdir(envdir)) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }

        if (lstat((tdir + ent->d_name).c_str(), &st)) {
            perror("stat");
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            res += sumup_dir(tdir + ent->d_name);
        } else if (S_ISREG(st.st_mode)) {
            res += st.st_size;
        }

        // else ignore
    }

    closedir(envdir);
    return res;
}

static void list_target_dirs(const string &current_target, const string &targetdir, Environments &envs)
{
    DIR *envdir = opendir(targetdir.c_str());

    if (!envdir) {
        return;
    }

    for (struct dirent *ent = readdir(envdir); ent; ent = readdir(envdir)) {
        string dirname = ent->d_name;

        if (access(string(targetdir + "/" + dirname + "/usr/bin/as").c_str(), X_OK) == 0) {
            envs.push_back(make_pair(current_target, dirname));
        }
    }

    closedir(envdir);
}

/* Returns true if the child exited with success */
static bool exec_and_wait(const char *const argv[])
{
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork");
        return false;
    }

    if (pid) {
        // parent
        int status;

        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

        return shell_exit_status(status) == 0;
    }

    // child
    execv(argv[0], const_cast<char * const *>(argv));
    log_perror("execv failed");
    _exit(-1);
}

// Removes everything in the directory recursively, but not the directory itself.
static bool cleanup_directory(const string &directory)
{
    DIR *dir = opendir(directory.c_str());

    if (dir == NULL) {
        return false;
    }

    while (dirent *f = readdir(dir)) {
        if (strcmp(f->d_name, ".") == 0 || strcmp(f->d_name, "..") == 0) {
            continue;
        }

        string fullpath = directory + '/' + f->d_name;
        struct stat st;

        if (lstat(fullpath.c_str(), &st)) {
            perror("stat");
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!cleanup_directory(fullpath) || rmdir(fullpath.c_str()) != 0) {
                return false;
            }
        } else {
            if (unlink(fullpath.c_str()) != 0) {
                return false;
            }
        }
    }

    closedir(dir);
    return true;
}

bool cleanup_cache(const string &basedir, uid_t user_uid, gid_t user_gid)
{
    flush_debug();

    if (access(basedir.c_str(), R_OK) == 0 && !cleanup_directory(basedir)) {
        log_error() << "failed to clean up envs dir" << endl;
        return false;
    }

    if (mkdir(basedir.c_str(), 0755) && errno != EEXIST) {
        if (errno == EPERM) {
            log_error() << "permission denied on mkdir " << basedir << endl;
        } else {
            log_perror("mkdir in cleanup_cache() failed") << "\t" << basedir << endl;
        }

        return false;
    }

    if (chown(basedir.c_str(), user_uid, user_gid) || chmod(basedir.c_str(), 0775)) {
        log_perror("chown/chmod in cleanup_cache() failed") << "\t" << basedir << endl;;
        return false;
    }

    return true;
}

Environments available_environmnents(const string &basedir)
{
    Environments envs;

    DIR *envdir = opendir(basedir.c_str());

    if (!envdir) {
        log_info() << "can't open envs dir " << strerror(errno) << endl;
    } else {
        for (struct dirent *target_ent = readdir(envdir); target_ent; target_ent = readdir(envdir)) {
            string dirname = target_ent->d_name;

            if (dirname.at(0) == '.') {
                continue;
            }

            if (dirname.substr(0, 7) == "target=") {
                string current_target = dirname.substr(7, dirname.length() - 7);
                list_target_dirs(current_target, basedir + "/" + dirname, envs);
            }
        }

        closedir(envdir);
    }

    return envs;
}

void save_compiler_timestamps(time_t &gcc_bin_timestamp, time_t &gpp_bin_timestamp, time_t &clang_bin_timestamp)
{
    struct stat st;

    if (stat("/usr/bin/gcc", &st) == 0) {
        gcc_bin_timestamp = st.st_mtime;
    } else {
        gcc_bin_timestamp = 0;
    }

    if (stat("/usr/bin/g++", &st) == 0) {
        gpp_bin_timestamp = st.st_mtime;
    } else {
        gpp_bin_timestamp = 0;
    }

    if (stat("/usr/bin/clang", &st) == 0) {
        clang_bin_timestamp = st.st_mtime;
    } else {
        clang_bin_timestamp = 0;
    }
}

bool compilers_uptodate(time_t gcc_bin_timestamp, time_t gpp_bin_timestamp, time_t clang_bin_timestamp)
{
    struct stat st;

    if (stat("/usr/bin/gcc", &st) == 0) {
        if (st.st_mtime != gcc_bin_timestamp) {
            return false;
        }
    } else {
        if (gcc_bin_timestamp != 0) {
            return false;
        }
    }

    if (stat("/usr/bin/g++", &st) == 0) {
        if (st.st_mtime != gpp_bin_timestamp) {
            return false;
        }
    } else {
        if (gpp_bin_timestamp != 0) {
            return false;
        }
    }

    if (stat("/usr/bin/clang", &st) == 0) {
        if (st.st_mtime != clang_bin_timestamp) {
            return false;
        }
    } else {
        if (clang_bin_timestamp != 0) {
            return false;
        }
    }

    return true;
}

// Returns fd for icecc-create-env output
int start_create_env(const string &basedir, uid_t user_uid, gid_t user_gid,
                     const std::string &compiler, const list<string> &extrafiles)
{
    string nativedir = basedir + "/native/";

    if (compiler == "clang") {
        if (::access("/usr/bin/clang", X_OK) != 0) {
            return 0;
        }
    } else { // "gcc" (the default)
        // Both gcc and g++ are needed in the gcc case.
        if (::access("/usr/bin/gcc", X_OK) != 0 || ::access("/usr/bin/g++", X_OK) != 0) {
            return 0;
        }
    }

    if (mkdir(nativedir.c_str(), 0775) && errno != EEXIST) {
        return 0;
    }

    if (chown(nativedir.c_str(), user_uid, user_gid) ||
            chmod(nativedir.c_str(), 0775)) {
        log_perror("chown/chmod failed");
        if (-1 == rmdir(nativedir.c_str())){
            log_perror("rmdir failed");
        }
        return 0;
    }

    flush_debug();
    int pipes[2];
    if (pipe(pipes) == -1) {
        log_error() << "failed to create pipe: " << strerror(errno) << endl;
        _exit(147);
    }
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork");
        _exit(147);
    }

    if (pid) {
        if ((-1 == close(pipes[1])) && (errno != EBADF)){
            log_perror("close failed");
        }
        return pipes[0];
    }
    // else

#ifndef HAVE_LIBCAP_NG
    if (getuid() != user_uid || geteuid() != user_uid
	|| getgid() != user_gid || getegid() != user_gid) {

        if (setgroups(0, NULL) < 0) {
            log_perror("setgroups failed");
            _exit(143);
        }

        if (setgid(user_gid) < 0) {
            log_perror("setgid failed");
            _exit(143);
        }

        if (!geteuid() && setuid(user_uid) < 0) {
            log_perror("setuid failed");
            _exit(142);
        }
    }
#endif

    if (chdir(nativedir.c_str())) {
        log_perror("chdir") << "\t" << nativedir << endl;
        _exit(1);
    }

    if ((-1 == close(pipes[0])) && (errno != EBADF)){
        log_perror("close failed");
    }

    if (-1 == dup2(pipes[1], 5)){   // icecc-create-env will write the hash there
        log_perror("dup2 failed");
    }

    if ((-1 == close(pipes[1])) && (errno != EBADF)){
        log_perror("close failed");
    }

    if ((-1 == close(STDOUT_FILENO)) && (errno != EBADF)){ // hide output from icecc-create-env
        log_perror("close failed");
    }

    const char **argv;
    argv = new const char*[4 + extrafiles.size()];
    int pos = 0;
    argv[pos++] = BINDIR "/icecc";
    argv[pos++] = "--build-native";
    const int first_to_free = pos;
    argv[pos++] = strdup(compiler.c_str());

    for (list<string>::const_iterator it = extrafiles.begin(); it != extrafiles.end(); ++it) {
        argv[pos++] = strdup(it->c_str());
    }

    argv[pos++] = NULL;

    if (!exec_and_wait(argv)) {
        log_error() << BINDIR "/icecc --build-native failed" << endl;
        _exit(1);
    }
    for( int i = first_to_free; i < pos; ++i )
        free( (void*) argv[ i ] );
    delete[] argv;

    _exit(0);
}

size_t finish_create_env(int pipe, const string &basedir, string &native_environment)
{
// We don't care about waitpid() , icecc-create-env prints the name of the tarball as the very last
// action before exit, so if there's something in the pipe, just block on it until it closes.

    char buf[1024];
    buf[0] = '\0';

    while (read(pipe, buf, 1023) < 0 && errno == EINTR) {}

    if (char *nl = strchr(buf, '\n')) {
        *nl = '\0';
    }

    string nativedir = basedir + "/native/";
    native_environment = nativedir + buf;

    if ((-1 == close(pipe)) && (errno != EBADF)){
        log_perror("close failed");
    }
    trace() << "native_environment " << native_environment << endl;
    struct stat st;

    if (!native_environment.empty()
        && (stat(native_environment.c_str(), &st) == 0)) {
        return st.st_size;
    }

    if (-1 == rmdir(nativedir.c_str())){
        log_perror("rmdir failed");
    }
    return 0;
}


pid_t start_install_environment(const std::string &basename, const std::string &target,
                                const std::string &name, MsgChannel *c,
                                int &pipe_to_stdin, FileChunkMsg *&fmsg,
                                uid_t user_uid, gid_t user_gid, int extract_priority)
{
    if (!name.size()) {
        log_error() << "illegal name for environment " << name << endl;
        return 0;
    }

    for (string::size_type i = 0; i < name.size(); ++i) {
        if (isascii(name[i]) && !isspace(name[i]) && name[i] != '/' && isprint(name[i])) {
            continue;
        }

        log_error() << "illegal char '" << name[i] << "' - rejecting environment " << name << endl;
        return 0;
    }

    string dirname = basename + "/target=" + target;
    Msg *msg = c->get_msg(30);

    if (!msg || msg->type != M_FILE_CHUNK) {
        trace() << "Expected first file chunk\n";
        return 0;
    }

    fmsg = dynamic_cast<FileChunkMsg*>(msg);
    const char *decompressor = NULL;

    if (fmsg->len > 2) {
        if (fmsg->buffer[0] == 037 && fmsg->buffer[1] == 0213) {
            decompressor = "-z";    // --gzip
        } else if (fmsg->buffer[0] == 'B' && fmsg->buffer[1] == 'Z') {
            decompressor = "-j";    // --bzip2
        } else if (fmsg->buffer[0] == 0xfd && fmsg->buffer[1] == 0x37) {
            decompressor = "-J";    // --xz
        } else if (fmsg->buffer[0] == 0x28 && fmsg->buffer[1] == 0xb5) {
            decompressor = "-Iunzstd";
        }
    }

    if (mkdir(dirname.c_str(), 0770) && errno != EEXIST) {
        log_perror("mkdir target") << "\t" << dirname << endl;
        return 0;
    }

    if (chown(dirname.c_str(), user_uid, user_gid) || chmod(dirname.c_str(), 0770)) {
        log_perror("chown,chmod target") << "\t" << dirname << endl;
        return 0;
    }

    dirname = dirname + "/" + name;

    if (mkdir(dirname.c_str(), 0770)) {
        log_perror("mkdir name") << "\t" << dirname << endl;
        return 0;
    }

    if (chown(dirname.c_str(), user_uid, user_gid) || chmod(dirname.c_str(), 0770)) {
        log_perror("chown,chmod name") << "\t" << dirname << endl;
        return 0;
    }

    int fds[2];

    if (pipe(fds) == -1) {
        log_perror("pipe failed");
        return 0;
    }

    flush_debug();
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("fork - trying to run tar");
        return 0;
    }
    if (pid) {
        trace() << "pid " << pid << endl;

        if ((-1 == close(fds[0])) && (errno != EBADF)){
            log_perror("close failed");
        }
        pipe_to_stdin = fds[1];

        return pid;
    }

    // else
#ifndef HAVE_LIBCAP_NG

    if (setgroups(0, NULL) < 0) {
        log_perror("setgroups fails");
        _exit(143);
    }

    if (setgid(user_gid) < 0) {
        log_perror("setgid fails");
        _exit(143);
    }

    if (!geteuid() && setuid(user_uid) < 0) {
        log_perror("setuid fails");
        _exit(142);
    }

#endif

    // reset SIGPIPE and SIGCHILD handler so that tar
    // isn't confused when gzip/bzip2 aborts
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    if ((-1 == close(0)) && (errno != EBADF)){
        log_perror("close failed");
    }

    if ((-1 == close(fds[1])) && (errno != EBADF)){
        log_perror("close failed");
    }

    if (-1 == dup2(fds[0], 0)){
        log_perror("dup2 failed");
    }

    int niceval = nice(extract_priority);
    if (-1 == niceval){
        log_warning() << "failed to set nice value: " << strerror(errno) << endl;
    }

    char **argv;
    argv = new char*[5];
    argv[0] = strdup(TAR);
    argv[1] = strdup("-xC");
    argv[2] = strdup(dirname.c_str());
    argv[3] = decompressor ? strdup(decompressor) : 0;
    argv[4] = 0;

    execv(argv[0], argv);
    log_perror("execv failed");
    _exit(100);
}


size_t finalize_install_environment(const std::string &basename, const std::string &target,
                                    pid_t pid, uid_t user_uid, gid_t user_gid)
{
    int status = 1;

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    if (shell_exit_status(status) != 0) {
        log_error() << "exit code: " << shell_exit_status(status) << endl;
        remove_environment(basename, target);
        return 0;
    }

    string dirname = basename + "/target=" + target;

    errno = 0;
    mkdir((dirname + "/tmp").c_str(), 01775);
    ignore_result(chown((dirname + "/tmp").c_str(), user_uid, user_gid));
    chmod((dirname + "/tmp").c_str(), 01775);
    if (errno == -1) {
        log_error() << "failed to setup " << dirname << "/tmp :"
                    << strerror(errno) << endl;
    }

    return sumup_dir(dirname);
}

size_t remove_environment(const string &basename, const string &env)
{
    string dirname = basename + "/target=" + env;

    size_t res = sumup_dir(dirname);

    flush_debug();
    pid_t pid = fork();

    if (pid == -1) {
        log_perror("failed to fork");
        return 0;
    }

    if (pid) {
        int status = 0;

        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

        if (WIFEXITED(status)) {
            return res;
        }

        // something went wrong. assume no disk space was free'd.
        return 0;
    }

    // else

    char **argv;
    argv = new char*[5];
    argv[0] = strdup("/bin/rm");
    argv[1] = strdup("-rf");
    argv[2] = strdup("--");
    argv[3] = strdup(dirname.c_str());
    argv[4] = NULL;

    execv(argv[0], argv);
    log_perror("execv failed");
    _exit(-1);
}

size_t remove_native_environment(const string &env)
{
    if (env.empty()) {
        return 0;
    }

    struct stat st;

    if (stat(env.c_str(), &st) == 0) {
        if (-1 == unlink(env.c_str())){
            log_perror("unlink failed") << "\t" << env << endl;
        }
        return st.st_size;
    }

    return 0;
}

static void
error_client(MsgChannel *client, string error)
{
    if (IS_PROTOCOL_23(client)) {
        client->send_msg(StatusTextMsg(error));
    }
}

void chdir_to_environment(MsgChannel *client, const string &dirname, uid_t user_uid, gid_t user_gid)
{
#ifdef HAVE_UNSHARE
    int flags = 0;
#  ifdef CLONE_NEWIPC
    flags |= CLONE_NEWIPC;
#  endif
#  ifdef CLONE_NEWNET
    flags |= CLONE_NEWNET;
#  endif
#  ifdef CLONE_NEWNS
    flags |= CLONE_NEWNS;    // mount namespace
#  endif
#  ifdef CLONE_NEWPID
    flags |= CLONE_NEWPID;
#  endif
#  ifdef CLONE_NEWUSER
    flags |= CLONE_NEWUSER;
#  endif
#  ifdef CLONE_NEWUTS
    flags |= CLONE_NEWUTS;
#  endif
    (void) unshare(flags);
#endif

#ifdef HAVE_LIBCAP_NG

    if (chdir(dirname.c_str()) < 0) {
        error_client(client, string("chdir to ") + dirname + "failed");
        log_perror("chdir() failed") << "\t" << dirname << endl;
        _exit(145);
    }

    if (chroot(dirname.c_str()) < 0) {
        error_client(client, string("chroot ") + dirname + "failed");
        log_perror("chroot() failed") << "\t" << dirname << endl;
        _exit(144);
    }

    (void) user_uid;
    (void) user_gid;
#else

    if (getuid() == 0) {
        // without the chdir, the chroot will escape the
        // jail right away
        if (chdir(dirname.c_str()) < 0) {
            error_client(client, string("chdir to ") + dirname + "failed");
            log_perror("chdir() failed") << "\t" << dirname << endl;
            _exit(145);
        }

        if (chroot(dirname.c_str()) < 0) {
            error_client(client, string("chroot ") + dirname + "failed");
            log_perror("chroot() failed") << "\t" << dirname << endl;
            _exit(144);
        }

        if (setgroups(0, NULL) < 0) {
            error_client(client, string("setgroups failed"));
            log_perror("setgroups() failed");
            _exit(143);
        }

        if (setgid(user_gid) < 0) {
            error_client(client, string("setgid failed"));
            log_perror("setgid() failed");
            _exit(143);
        }

        if (setuid(user_uid) < 0) {
            error_client(client, string("setuid failed"));
            log_perror("setuid() failed");
            _exit(142);
        }
    } else {
        error_client(client, "cannot chroot to environment");
        _exit(146);
    }

#endif
}

// Verify that the environment works by simply running the bundled bin/true.
bool verify_env(MsgChannel *client, const string &basedir, const string &target, const string &env,
                uid_t user_uid, gid_t user_gid)
{
    if (target.empty() || env.empty()) {
        error_client(client, "verify_env: target or env empty");
        log_error() << "verify_env target or env empty\n\t" << target << "\n\t" << env << endl;
        return false;
    }

    string dirname = basedir + "/target=" + target + "/" + env;

    if (::access(string(dirname + "/bin/true").c_str(), X_OK) < 0) {
        error_client(client, dirname + "/bin/true is not executable, installed environment removed?");
        log_error() << "I don't have environment " << env << "(" << target << ") to verify." << endl;
        return false;
    }

    flush_debug();
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid > 0) {  // parent
        int status;

        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

        return shell_exit_status(status) == 0;
    } else if (pid < 0) {
        log_perror("fork failed");
        return false;
    }

    // child
    reset_debug();
    chdir_to_environment(client, dirname, user_uid, user_gid);
    execl("bin/true", "bin/true", (void*)NULL);
    log_perror("execl failed");
    _exit(-1);

}
