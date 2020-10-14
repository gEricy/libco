/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <queue>
#include "co_routine.h"
using namespace std;
struct stTask_t
{
	int id;
};
struct stEnv_t
{
	stCoCond_t* cond;  // 条件变量(本质：双向链表)
	queue<stTask_t*> task_queue;  // 任务队列
};
void* Producer(void* args)
{
	co_enable_hook_sys();
	stEnv_t* env=  (stEnv_t*)args;
	int id = 0;
	while (true)
	{
		stTask_t* task = (stTask_t*)calloc(1, sizeof(stTask_t)); // 创建一个任务, 添加到任务队列
		task->id = id++;
		env->task_queue.push(task);
		printf("%s:%d produce task %d\n", __func__, __LINE__, task->id);
		co_cond_signal(env->cond); // (4) 唤醒在条件变量等待的协程, 将会切换到(2)
		poll(NULL, 0, 1000);  // (5) 调用poll, 将生产者协程设置超时时间, 然后切走
	}
	return NULL;
}
void* Consumer(void* args)
{
	co_enable_hook_sys();
	stEnv_t* env = (stEnv_t*)args;
	while (true)
	{
		if (env->task_queue.empty()) // 队列为空
		{
			co_cond_timedwait(env->cond, -1);  // (2) 在条件变量等待, 切回主协程
			continue;
		}
		stTask_t* task = env->task_queue.front();  // (6) 从队列中取出任务, 执行任务, 执行完后会再次进入步骤(2)等待
		env->task_queue.pop();
		printf("%s:%d consume task %d\n", __func__, __LINE__, task->id);
		free(task);
	}
	return NULL;
}

stEnv_t* stEnv_init()
{
	stEnv_t* env = new stEnv_t;
	env->cond = co_cond_alloc();  // 创建条件变量
}
int main()
{
	stEnv_t* env = stEnv_init();
	
	stCoRoutine_t* consumer_routine;
	stCoRoutine_t* producer_routine;

	// (1) 创建消费者协程，切换到该协程Consumer执行
	co_create(&consumer_routine, NULL, Consumer, env); 
	co_resume(consumer_routine);

	// (3) 创建生产者协程, 切换到该协程Producer执行
	co_create(&producer_routine, NULL, Producer, env);
	co_resume(producer_routine);
	
	co_eventloop(co_get_epoll_ct(), NULL, NULL);
	return 0;
}
