#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <ctime>
#include <string>
#include <sstream>
#include <map>
#include <queue>
#ifdef __MINGW32__
#include <windows.h>

//from: http://kbokonseriousstuff.blogspot.com/2011/08/c11-standard-library-threads-with-mingw.html
#include <boost/thread.hpp>
namespace std {
	using boost::mutex;
	using boost::recursive_mutex;
	using boost::lock_guard;
	using boost::condition_variable;
	using boost::unique_lock;
	using boost::thread;
}
#else
#include <mutex>
#include <thread>
#include <condition_variable>
#endif

#include "judge_daemon.h"

std::map<std::string, solution* > finder;
std::queue<solution*> waiting, removing;
std::mutex waiting_mutex, removing_mutex, finder_mutex, applog_mutex;
std::condition_variable notifier;

#ifdef USE_DLL_ON_WINDOWS
run_compiler_def run_compiler;
run_judge_def run_judge;
#endif
extern char target_path[];

solution::solution()
{
	mutex_for_query = new std::mutex;
}
solution::~solution()
{
	delete (std::mutex*)mutex_for_query;
}
void applog(const char *str)
{
	time_t now_time = time(NULL);
	std::unique_lock<std::mutex> Lock(applog_mutex);
	char time_str[24];
#ifdef __MINGW32__
	std::strftime(time_str, 24, "%Y-%m-%d %H:%M:%S", std::localtime(&now_time));
#else
	std::strftime(time_str, 24, "%F %T", std::localtime(&now_time));
#endif
	printf("[%s] %s\n", time_str, str);
	fflush(stdout);
}
void json_builder(std::ostringstream &, solution *);
char* JUDGE_get_progress(const char *query)
{
	try {
		solution *target;
		do {
			std::unique_lock<std::mutex> Lock_map(finder_mutex);
			auto it = finder.find(std::string(query + 7));//query must start with "query_",checked in http.cpp
			if(finder.end() == it) {
				char *response = (char*)malloc(21);
				if(response)
					strcpy(response, "{\"state\":\"invalid\"}");
				return response;
			}
			target = it->second;
		}while(0);

		std::ostringstream json;
		do {
			std::unique_lock<std::mutex> Lock_map(* (std::mutex*)(target->mutex_for_query));
			json_builder(json, target);
		}while(0);

		const std::string &buf = json.str();
		char *response = (char*)malloc(buf.size() + 1);
		if(response)
			strcpy(response, buf.c_str());
		return response;
	}catch(...) {
		applog("Info: Exception when query");
	}
	char *response = (char*)malloc(21);
	if(response)
		strcpy(response, "{\"state\":\"invalid\"}");
	return response;
}
void thread_remove()
{
	for(;;) {
		std::unique_lock<std::mutex> Lock(removing_mutex);
		if(removing.empty()) {
			//puts("empty");
			Lock.unlock();
#ifdef _WIN32
			Sleep(20000);
#else
			sleep(20);
#endif
			//std::this_thread::sleep_for(std::chrono::milliseconds(2000));
			continue;
		}
		time_t now = time(0);
		if((now - removing.front()->timestamp) < 25) {
			//printf("%d - %d < 2\n", now, removing.front()->timestamp);
			Lock.unlock();
			int remain = (now - removing.front()->timestamp) + 1;
#ifdef _WIN32
			Sleep(remain*1000);
#else
			sleep(remain);
#endif
			//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			continue;
		}
		puts("Information deleted");
		solution *cur = removing.front();
		finder.erase(cur->key);
		removing.pop();
#ifdef _WIN32
		Sleep(1500);
#else
		sleep(1);
#endif
		//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		delete cur;
	}
}
void thread_judge()
{
	for(;;) {
		solution *cur;
		do {
			std::unique_lock<std::mutex> Lock(waiting_mutex);
			notifier.wait(Lock, []()->bool {return !waiting.empty();});
			cur = waiting.front();
			waiting.pop();
		}while(0);

		try {
			if(!clean_files()) {
				applog("Info: Cannot clean working directory.");
			}
			if(cur->compile())
				cur->judge();
			cur->write_database();
			cur->timestamp = time(0);
		}catch(int) {
			std::string message("Error: Exception occur when judging ");
			message += cur->key; 
			applog(message.c_str());

			try {
				std::unique_lock<std::mutex> Lock_map(* (std::mutex*)(cur->mutex_for_query));
				cur->detail_results.clear();
				cur->detail_results.push_back({RES_SE, 0, 0, cur->last_state, 0});
				cur->timestamp = time(0);
			}catch(...) {
				;
			}
		}

		std::unique_lock<std::mutex> Lock(removing_mutex);
		removing.push(cur);
	}
}
char *JUDGE_accept_submit(solution *&new_sol)
{
	try {
		if(!new_sol)
			throw 1;
		new_sol->error_code = -1;
		new_sol->timestamp = 0;
		do {
			std::unique_lock<std::mutex> Lock(waiting_mutex);
			waiting.push(new_sol);
			notifier.notify_one();
		}while(0);
		do {
			std::unique_lock<std::mutex> Lock(finder_mutex);
			finder[new_sol->key] = new_sol;
		}while(0);
	}catch(...) {
		applog("Error: Exception in JUDGE_accept_submit()");
		char *p = (char*)malloc(6);
		strcpy(p, "error");
		return p;
	}
	new_sol = NULL;//avoid being freed
	char *p = (char*)malloc(3);
	strcpy(p, "OK");
	return p;
}
//TODO: watchdog
int main(int argc, char **argv)
{
	//enter program directory to read ini files
#if defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
	int size = GetModuleFileNameA(NULL, target_path, MAXPATHLEN);
	if(size <= 0) {
		applog("Error: Cannot get program directory, Exit...");
		exit(1);
	}
	for(int i=size-1; i>=0; i--)
		if(target_path[i] == '\\') {
			target_path[i+1] = '\0';
			break;
		}
	printf("entering %s\n", target_path);
	if(!SetCurrentDirectory(target_path)) {
		applog("Error: Cannot enter program directory, Exit...");
		exit(1);
	}
#else
	int size = readlink("/proc/self/exe", target_path, MAXPATHLEN);
	if(size <= 0) {
		applog("Error: Cannot get program directory, Exit...");
		exit(1);
	}
	target_path[size] = '\0';
	for(int i=size-1; i>=0; i--)
		if(target_path[i] == '/') {
			target_path[i+1] = '\0';
			break;
		}
	printf("entering %s\n", target_path);
	if(0 != chdir(target_path)) {
		applog("Error: Cannot enter program directory, Exit...");
		exit(1);
	}
#endif
#else
	// I don't know
#endif

	if(!read_config()) {
		applog("Error: Cannot read and parse the config.ini, Exit...");
		exit(1);
	}
	if(!init_mysql_con()) {
		applog("Error: Cannot connect to mysql, Exit...");
		exit(1);
	}
#ifdef __MINGW32__
	mkdir("temp");
#else
	mkdir("temp", 0777);
#endif
	if(0 != chdir("temp")) {
		applog("Error: Cannot enter working directory, Exit...");
		exit(1);
	}
#ifndef __MINGW32__ //used when run program on *nix only
	if(NULL == getcwd(target_path, MAXPATHLEN)) {
		applog("Error: Cannot get working directory, Exit...");
		exit(1);
	}
	strcat(target_path, "/target.exe");
	printf("target: %s\n", target_path);
#else
	strcpy(target_path,"target.exe");
#endif

	std::thread thread_j(thread_judge);
	std::thread thread_r(thread_remove);

	if(!start_http_interface()) {
		applog("Error: Cannot open http interface, Exit...");
		exit(1);
	}
	applog("Started successfully.Waiting for submitting...");
	thread_j.join();
}