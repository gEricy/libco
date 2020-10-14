/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited,  a Tencent company. All rights reserved.
*
* Licensed under the Apache License,  Version 2.0 (the "License");  
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,  
* software distributed under the License is distributed on an "AS IS" BASIS,  
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,  either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

extern "C"
{
	extern void coctx_swap( coctx_t *, coctx_t* ) asm("coctx_swap"); 
}; 
using namespace std; 
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env ); 
struct stCoEpoll_t; 

struct stCoRoutineEnv_t
{
	stCoRoutine_t *pCallStack[ 128 ];   // 协程调用栈(栈顶元素始终是当前正在运行的协程)
	int iCallStackSize;    // 协程栈中协程的个数

	stCoEpoll_t *pEpoll;   // Epoll管理者

	//for copy stack log lastco and nextco
	stCoRoutine_t* pending_co; 
	stCoRoutine_t* occupy_co; 
}; 
//int socket(int domain,  int type,  int protocol); 
void co_log_err( const char *fmt, ... )
{
}


#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
	register uint32_t lo,  hi; 
	register unsigned long long o; 
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo),  "=d"(hi)::"%rcx"
			); 
	o = hi; 
	o <<= 32; 
	return (o | lo); 

}
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo", "r"); 
	if(!fp) return 1; 
	char buf[4096] = {0}; 
	fread(buf, 1, sizeof(buf), fp); 
	fclose(fp); 

	char *lp = strstr(buf, "cpu MHz"); 
	if(!lp) return 1; 
	lp += strlen("cpu MHz"); 
	while(*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp; 
	}

	double mhz = atof(lp); 
	unsigned long long u = (unsigned long long)(mhz * 1000); 
	return u; 
}
#endif

static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
	static uint32_t khz = getCpuKhz(); 
	return counter() / khz; 
#else
	struct timeval now = { 0 }; 
	gettimeofday( &now, NULL ); 
	unsigned long long u = now.tv_sec; 
	u *= 1000; 
	u += now.tv_usec / 1000; 
	return u; 
#endif
}

template <class T, class TLink>
void RemoveFromLink(T *ap)  // 将ap节点从它所属的链表中删除
{
	TLink *lst = ap->pLink;  // 链表首尾结构体
	if(!lst) 
		return ; 
	
	assert( lst->head && lst->tail ); 

	if( ap == lst->head )
	{
		lst->head = ap->pNext; 
		if(lst->head)
		{
			lst->head->pPrev = NULL; 
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext; 
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev; 
		if(lst->tail)
		{
			lst->tail->pNext = NULL; 
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev; 
	}

	ap->pPrev = ap->pNext = NULL; 
	ap->pLink = NULL; 
}

template <class TNode, class TLink>
void inline AddTail(TLink*apLink, TNode *ap)  // ap添加到apLink链表的尾部
{
	if( ap->pLink )
	{
		return ; 
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap; 
		ap->pNext = NULL; 
		ap->pPrev = apLink->tail; 
		apLink->tail = ap; 
	}
	else
	{
		apLink->head = apLink->tail = ap; 
		ap->pNext = ap->pPrev = NULL; 
	}
	ap->pLink = apLink; 
}

template <class TNode, class TLink>
void inline PopHead( TLink*apLink )  // 弹出apLink链表的头(第一个元素)
{
	if( !apLink->head ) 
	{
		return ; 
	}
	TNode *lp = apLink->head; 
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL; 
	}
	else
	{
		apLink->head = apLink->head->pNext; 
	}

	lp->pPrev = lp->pNext = NULL; 
	lp->pLink = NULL; 

	if( apLink->head )
	{
		apLink->head->pPrev = NULL; 
	}
}


// 将apOther链表，添加到apLink链表, 组成一个新的链表
template <class TNode, class TLink>
void inline Join( TLink*apLink, TLink *apOther )  
{
	//printf("apOther %p\n", apOther); 
	if( !apOther->head )
	{
		return ; 
	}
	
	TNode *lp = apOther->head; 
	while( lp )
	{
		lp->pLink = apLink; 
		lp = lp->pNext; 
	}
	lp = apOther->head; 
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp; 
		lp->pPrev = apLink->tail; 
		apLink->tail = apOther->tail; 
	}
	else
	{
		apLink->head = apOther->head; 
		apLink->tail = apOther->tail; 
	}

	apOther->head = apOther->tail = NULL; 
}

/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t)); 
	stack_mem->occupy_co= NULL; 
	stack_mem->stack_size = stack_size; 
	stack_mem->stack_buffer = (char*)malloc(stack_size); 
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size; 
	return stack_mem; 
}

stShareStack_t* co_alloc_sharestack(int count,  int stack_size)
{
	stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t)); 
	share_stack->alloc_idx = 0; 
	share_stack->stack_size = stack_size; 

	//alloc stack array
	share_stack->count = count; 
	stStackMem_t** stack_array = (stStackMem_t**)calloc(count,  sizeof(stStackMem_t*)); 
	for (int i = 0;  i < count;  i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size); 
	}
	share_stack->stack_array = stack_array; 
	return share_stack; 
}

static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
	if (!share_stack)
	{
		return NULL; 
	}
	int idx = share_stack->alloc_idx % share_stack->count; 
	share_stack->alloc_idx++; 

	return share_stack->stack_array[idx]; 
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t; 
struct stTimeoutItem_t; 

// epoll事件管理者: 超时事件\激活事件
struct stCoEpoll_t  
{
	int iEpollFd;   // 红黑树的树根

	static const int _EPOLL_SIZE = 1024 * 10; 

	struct stTimeout_t *pTimeout;   // 时间轮 (数组: 元素是双向链表)

	struct stTimeoutItemLink_t *pstTimeoutList;   // 链表头：超时事件
	struct stTimeoutItemLink_t *pstActiveList;    // 链表头：激活事件 (每次事件到来, 都将清空该链表上, 触发注册的事件)

	co_epoll_res *result;   // epoll_wait激活事件数组
}; 
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *, struct epoll_event &ev,  stTimeoutItemLink_t *active ); 
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *); 


// 每个超时结构体
struct stTimeoutItem_t
{
	enum{
		eMaxTimeout = 40 * 1000 //40s
	}; 

	// 相当于struct list_head, 用来串成一个双向链表 (表示该stTimeoutItem_t结构体, 是双向链表中的一个元素)
	stTimeoutItem_t *pPrev; 
	stTimeoutItem_t *pNext; 

	// 双向链表的首尾指针
	stTimeoutItemLink_t *pLink; 

	unsigned long long ullExpireTime; 

	OnPreparePfn_t pfnPrepare;   // 预处理
	OnProcessPfn_t pfnProcess;   // 事件触发执行的函数

	void *pArg;  // routine 保存了该超时结构体的协程co, 用于超时时, 切换到co执行

	bool bTimeout;  // 是否超时的标记
}; 

// 双向链表的首尾指针
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head; 
	stTimeoutItem_t *tail; 
}; 

// 时间轮
struct stTimeout_t  
{
	stTimeoutItemLink_t *pItems;   // 时间轮数组 (数组: 每个元素是双向链表的链表头)
	int iItemSize;                 //    双向链表中的元素: stTimeoutItem_t(超时结构体)

	unsigned long long ullStart; 
	long long llStartIdx;   // 指向时间轮最新时间下标
}; 

/* 时间轮函数: 很简单, 此处不再叙述 */
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1, sizeof(stTimeout_t) ); 	

	lp->iItemSize = iSize; 
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1, sizeof(stTimeoutItemLink_t) * lp->iItemSize ); 

	lp->ullStart = GetTickMS(); 
	lp->llStartIdx = 0; 

	return lp; 
}
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems ); 
	free ( apTimeout ); 
}

// 将apItem根据超时时间allNow添加到apTimeout链表中
int AddTimeout( stTimeout_t *apTimeout, stTimeoutItem_t *apItem , unsigned long long allNow )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow; 
		apTimeout->llStartIdx = 0; 
	}
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu", 
					__LINE__, allNow, apTimeout->ullStart); 

		return __LINE__; 
	}
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu", 
					__LINE__, apItem->ullExpireTime, allNow, apTimeout->ullStart); 

		return __LINE__; 
	}
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart; 

	if( diff >= (unsigned long long)apTimeout->iItemSize )
	{
		diff = apTimeout->iItemSize - 1; 
		co_log_err("CO_ERR: AddTimeout line %d diff %d", 
					__LINE__, diff); 

		//return __LINE__; 
	}
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize ,  apItem ); 

	return 0; 
}

// 从apTimeout链表, 取出超过allNow的时间, 保存到双向链表apResult中
inline void TakeAllTimeout( stTimeout_t *apTimeout, unsigned long long allNow, stTimeoutItemLink_t *apResult )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow; 
		apTimeout->llStartIdx = 0; 
	}

	if( allNow < apTimeout->ullStart )
	{
		return ; 
	}
	int cnt = allNow - apTimeout->ullStart + 1; 
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize; 
	}
	if( cnt < 0 )
	{
		return; 
	}
	for( int i = 0; i<cnt; i++)
	{
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize; 
		Join<stTimeoutItem_t, stTimeoutItemLink_t>( apResult, apTimeout->pItems + idx  ); 
	}
	apTimeout->ullStart = allNow; 
	apTimeout->llStartIdx += cnt - 1; 
}

// 协程绑定的上下文函数, 该函数会执行注册的协程函数, 且执行完后, 再会切回到之前的协程
static int CoRoutineFunc( stCoRoutine_t *co, void * )
{
	if( co->pfn )  // 将会执行用户注册的协程函数
	{
		co->pfn( co->arg ); 
	}
	co->cEnd = 1;   // 协程函数执行完后, 将标记位设置为cEnd

	stCoRoutineEnv_t *env = co->env; 


	// 该协程执行完成后, 要在协程调用栈中删除该协程, 切回上次的协程(curr --> last)
	co_yield_env( env );   

	return 0; 
}


// co_create会调用该函数: (1) 创建协程结构体 (2) 创建协程结构体成员变量赋值 (3) 返回创建的协程结构体
struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env,  const stCoRoutineAttr_t* attr, 
		pfn_co_routine_t pfn, void *arg )
{

	stCoRoutineAttr_t at; 
	if( attr )
	{
		memcpy( &at, attr, sizeof(at) ); 
	}
	if( at.stack_size <= 0 )
	{
		at.stack_size = 128 * 1024; 
	}
	else if( at.stack_size > 1024 * 1024 * 8 )
	{
		at.stack_size = 1024 * 1024 * 8; 
	}

	if( at.stack_size & 0xFFF ) 
	{
		at.stack_size &= ~0xFFF; 
		at.stack_size += 0x1000; 
	}

	// 创建协程结构体
	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) ); 
	
	memset( lp, 0, (long)(sizeof(stCoRoutine_t)));  

	lp->env = env; 
	lp->pfn = pfn; 
	lp->arg = arg; 

	stStackMem_t* stack_mem = NULL; 
	if( at.share_stack )
	{
		stack_mem = co_get_stackmem( at.share_stack); 
		at.stack_size = at.share_stack->stack_size; 
	}
	else
	{
		stack_mem = co_alloc_stackmem(at.stack_size); 
	}
	lp->stack_mem = stack_mem; 

	lp->ctx.ss_sp = stack_mem->stack_buffer; 
	lp->ctx.ss_size = at.stack_size; 

	lp->cStart = 0; 
	lp->cEnd = 0; 
	lp->cIsMain = 0; 
	lp->cEnableSysHook = 0; 
	lp->cIsShareStack = at.share_stack != NULL; 

	lp->save_size = 0; 
	lp->save_buffer = NULL; 

	return lp; 
}

// 创建协程, 注册协程函数\参数
int co_create( stCoRoutine_t **ppco, const stCoRoutineAttr_t *attr, pfn_co_routine_t pfn, void *arg )
{
	if( !co_get_curr_thread_env() ) 
	{
		co_init_curr_thread_env(); 
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(),  attr,  pfn, arg ); 
	*ppco = co; 
	return 0; 
}
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer); 
        free(co->stack_mem); 
    }   
    //walkerdu fix at 2018-01-20
    //存在内存泄漏
    else 
    {
        if(co->save_buffer)
            free(co->save_buffer); 

        if(co->stack_mem->occupy_co == co)
            co->stack_mem->occupy_co = NULL; 
    }

    free( co ); 
}
void co_release( stCoRoutine_t *co )
{
    co_free( co ); 
}

void co_swap(stCoRoutine_t* curr,  stCoRoutine_t* pending_co); 

void co_resume( stCoRoutine_t *co )
{
	stCoRoutineEnv_t *env = co->env; 
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];  // 正在运行的协程 (栈顶)
	if( !co->cStart )
	{
		coctx_make( &co->ctx, (coctx_pfn_t)CoRoutineFunc, co, 0 ); 
		co->cStart = 1; 
	}
	
	env->pCallStack[ env->iCallStackSize++ ] = co; 
	co_swap( lpCurrRoutine,  co );   // lpCurrRoutine切换到协程co绑定的上下文函数(CoRoutineFunc)
}


// walkerdu 2018-01-14                                                                              
// 用于reset超时无法重复使用的协程                                                                  
void co_reset(stCoRoutine_t * co)
{
    if(!co->cStart || co->cIsMain)
        return; 

    co->cStart = 0; 
    co->cEnd = 0; 

    // 如果当前协程有共享栈被切出的buff，要进行释放
    if(co->save_buffer)
    {
        free(co->save_buffer); 
        co->save_buffer = NULL; 
        co->save_size = 0; 
    }

    // 如果共享栈被当前协程占用，要释放占用标志，否则被切换，会执行save_stack_buffer()
    if(co->stack_mem->occupy_co == co)
        co->stack_mem->occupy_co = NULL; 
}


// 删除curr, curr --> last
void co_yield_env( stCoRoutineEnv_t *env )
{
	
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ]; 
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ]; 

	env->iCallStackSize--;   // 删除当前正在执行的协程任务

	co_swap( curr,  last);   // 当前正在执行的协程curr, 切换到上次执行的协程last
}

void co_yield_ct()
{
	co_yield_env( co_get_curr_thread_env() ); 
}
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env ); 
}

void save_stack_buffer(stCoRoutine_t* occupy_co)
{
	///copy out
	stStackMem_t* stack_mem = occupy_co->stack_mem; 
	int len = stack_mem->stack_bp - occupy_co->stack_sp; 

	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer),  occupy_co->save_buffer = NULL; 
	}

	occupy_co->save_buffer = (char*)malloc(len);  //malloc buf; 
	occupy_co->save_size = len; 

	memcpy(occupy_co->save_buffer,  occupy_co->stack_sp,  len); 
}

// 从curr切换到pending_to, 如果想切回来, 还要手动调用co_swap
static void co_swap(stCoRoutine_t* curr,  stCoRoutine_t* pending_co)
{
 	stCoRoutineEnv_t* env = co_get_curr_thread_env(); 

	//get curr stack sp
	char c; 
	curr->stack_sp= &c; 

	if (!pending_co->cIsShareStack)
	{
		env->pending_co = NULL; 
		env->occupy_co = NULL; 
	}
	else 
	{
		env->pending_co = pending_co; 
		//get last occupy co on the same stack mem
		stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co; 
		//set pending co to occupy thest stack mem; 
		pending_co->stack_mem->occupy_co = pending_co; 

		env->occupy_co = occupy_co; 
		if (occupy_co && occupy_co != pending_co)
		{
			save_stack_buffer(occupy_co); 
		}
	}

	//swap context   从curr切换到pending_co, 如果想切回来, 还需要手动调用coctx_swap
	coctx_swap(&(curr->ctx), &(pending_co->ctx) ); 

	//stack buffer may be overwrite,  so get again; 
	stCoRoutineEnv_t* curr_env = co_get_curr_thread_env(); 
	stCoRoutine_t* update_occupy_co =  curr_env->occupy_co; 
	stCoRoutine_t* update_pending_co = curr_env->pending_co; 
	
	if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
	{
		//resume stack buffer
		if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
		{
			memcpy(update_pending_co->stack_sp,  update_pending_co->save_buffer,  update_pending_co->save_size); 
		}
	}
}



// poll事件对象
struct stPollItem_t ; 
struct stPoll_t : public stTimeoutItem_t      // 继承超时结构体
{
	struct pollfd *fds;   // 数组 (一个poll事件, 可能绑定多个fd)
	nfds_t nfds;  

	stPollItem_t *pPollItems;   // 指向epoll事件对象

	int iAllEventDetach; 

	int iEpollFd;   // epfd

	int iRaiseCnt; 
}; 

// epoll事件对象
struct stPollItem_t : public stTimeoutItem_t  // 继承超时结构体
{
	struct pollfd *pSelf; 
	stPoll_t *pPoll;       // 指向poll事件对象

	struct epoll_event stEvent; 
}; 

// poll事件和epoll事件标志的转换
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0; 	
	if( events & POLLIN ) 	e |= EPOLLIN; 
	if( events & POLLOUT )  e |= EPOLLOUT; 
	if( events & POLLHUP ) 	e |= EPOLLHUP; 
	if( events & POLLERR )	e |= EPOLLERR; 
	if( events & POLLRDNORM ) e |= EPOLLRDNORM; 
	if( events & POLLWRNORM ) e |= EPOLLWRNORM; 
	return e; 
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0; 	
	if( events & EPOLLIN ) 	e |= POLLIN; 
	if( events & EPOLLOUT ) e |= POLLOUT; 
	if( events & EPOLLHUP ) e |= POLLHUP; 
	if( events & EPOLLERR ) e |= POLLERR; 
	if( events & EPOLLRDNORM ) e |= POLLRDNORM; 
	if( events & EPOLLWRNORM ) e |= POLLWRNORM; 
	return e; 
}

// 协程环境env
static __thread stCoRoutineEnv_t* gCoEnvPerThread = NULL; 

// 初始化协程环境
void co_init_curr_thread_env()
{
	gCoEnvPerThread = (stCoRoutineEnv_t*)calloc( 1,  sizeof(stCoRoutineEnv_t) ); 
	stCoRoutineEnv_t *env = gCoEnvPerThread; 

	env->iCallStackSize = 0; 

	struct stCoRoutine_t *self = co_create_env( env,  NULL,  NULL, NULL );   // 创建主协程的协程结构图
	self->cIsMain = 1; 
	coctx_init( &self->ctx ); 

	env->pending_co = NULL; 
	env->occupy_co = NULL; 
	env->pCallStack[ env->iCallStackSize++ ] = self; 

	// env->pEpoll
	stCoEpoll_t *ev = AllocEpoll(); 
	SetEpoll( env, ev ); 
}

stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return gCoEnvPerThread; 
}

// epoll_wait返回后, 触发的事件注册的回调函数
void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;   // 注册事件的协程上下文co
	co_resume( co );   // 切换到事件的协程co, 执行注册的协程函数(执行完会再切回)
}


// 预处理
void OnPollPreparePfn( stTimeoutItem_t * ap, struct epoll_event &e, 
								stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap; 
	lp->pSelf->revents = EpollEvent2Poll( e.events ); 

	stPoll_t *pPoll = lp->pPoll; 
	pPoll->iRaiseCnt++; 

	if( !pPoll->iAllEventDetach )
	{
		pPoll->iAllEventDetach = 1; 

		RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>( pPoll );  // 将“poll事件对象”从超时链表中【摘除】

		AddTail( active, pPoll );   // 添加到激活队列active
	}
}

// 监控epoll树上注册的事件
void co_eventloop( stCoEpoll_t *ctx, pfn_co_eventloop_t pfn, void *arg )
{
	if( !ctx->result )
	{
		// 创建数组, 保存epoll_wait返回的结果
		ctx->result =  co_epoll_res_alloc( stCoEpoll_t::_EPOLL_SIZE ); 
	}

	co_epoll_res *result = ctx->result; 


	for(; ; )
	{
		// epoll_wait监听事件的到来, 保存到result中
		int ret = co_epoll_wait( ctx->iEpollFd, result, stCoEpoll_t::_EPOLL_SIZE,  1 );  

		stTimeoutItemLink_t *active = (ctx->pstActiveList); 
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList); 

		memset( timeout, 0, sizeof(stTimeoutItemLink_t) ); 

		for(int i=0; i<ret; i++)  // 遍历每个激活的事件
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr; 

			// 将激活事件添加到激活队列active中
			if( item->pfnPrepare )
			{
				item->pfnPrepare( item, result->events[i], active ); 
			}
			else
			{
				AddTail( active, item ); 
			}
		}

		// 取出所有的超时事件, 保存到超时队列timeout中
		unsigned long long now = GetTickMS(); 
		TakeAllTimeout( ctx->pTimeout, now, timeout ); 

		stTimeoutItem_t *lp = timeout->head; 
		while( lp )
		{
			lp->bTimeout = true; // 给每个事件的超时标记设为true
			lp = lp->pNext; 
		}

		// 将超时队列timeout, 合并到激活队列active
		Join<stTimeoutItem_t, stTimeoutItemLink_t>( active, timeout ); 

		// 循环遍历所有的激活事件active
		lp = active->head; 
		while( lp )
		{
			PopHead<stTimeoutItem_t, stTimeoutItemLink_t>( active ); 
            if (lp->bTimeout && now < lp->ullExpireTime)
			{
				int ret = AddTimeout(ctx->pTimeout,  lp,  now); 
				if (!ret) 
				{
					lp->bTimeout = false; 
					lp = active->head; 
					continue; 
				}
			}
			if( lp->pfnProcess )  // 触发该事件注册的激活函数(OnPollProcessEvent): 即切换到该事件的协程中去
			{				      //      当注册的协程激活函数执行完成后, 还会立即切回来！
				lp->pfnProcess( lp ); 
			}

			lp = active->head; 
		}
		if( pfn )
		{
			if( -1 == pfn( arg ) )
			{
				break; 
			}
		}

	}
}

// 创建epoll事件管理者
stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1, sizeof(stCoEpoll_t) ); 

	ctx->iEpollFd = co_epoll_create( stCoEpoll_t::_EPOLL_SIZE ); 
	ctx->pTimeout = AllocTimeout( 60 * 1000 ); 
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1, sizeof(stTimeoutItemLink_t) ); 
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1, sizeof(stTimeoutItemLink_t) ); 

	return ctx; 
}

void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList ); 
		free( ctx->pstTimeoutList ); 
		FreeTimeout( ctx->pTimeout ); 
		co_epoll_res_free( ctx->result ); 
	}
	free( ctx ); 
}

// 获取env的当前正在执行的协程
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ]; 
}

// 获取当前正在执行的协程
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env(); 
	if( !env ) return 0; 
	return GetCurrCo(env); 
}


/*
 * @brief  该函数用来添加一个read\write事件
 *            添加完事件后, 将会切走协程
 */
typedef int (*poll_pfn_t)(struct pollfd fds[],  nfds_t nfds,  int timeout); 
int co_poll_inner( stCoEpoll_t *ctx, struct pollfd fds[],  nfds_t nfds,  int timeout,  poll_pfn_t pollfunc)
{
    if (timeout == 0)  // timeout永远不为0
	{
		return pollfunc(fds,  nfds,  timeout); 
	}
	if (timeout < 0)
	{
		timeout = INT_MAX; 
	}
	
	int epfd = ctx->iEpollFd; 
	stCoRoutine_t* self = co_self(); 

	//1. struct change  结构体转化
	stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t))); 
	memset( &arg, 0, sizeof(arg) ); 

	arg.iEpollFd = epfd; 
	arg.fds = (pollfd*)calloc(nfds,  sizeof(pollfd));  // poll: 该事件可能绑定多个文件描述符fds
	arg.nfds = nfds; 

	stPollItem_t arr[2]; 
	if( nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack)  // 只有一个fd && 不是共享栈
	{
		arg.pPollItems = arr; 
	}	
	else  // 多个fd
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );  // 创建多个epoll_event
	}
	memset( arg.pPollItems, 0, nfds * sizeof(stPollItem_t) ); 

	arg.pfnProcess = OnPollProcessEvent;  // 事件触发后, 执行的回调函数
	arg.pArg = GetCurrCo( co_get_curr_thread_env() );  // 回调函数的参数是当前的协程co, 方便执行完事件后, 切回当前的协程
	
	//2. add epoll  添加epoll事件
	for(nfds_t i=0; i<nfds; i++)
	{
		arg.pPollItems[i].pSelf = arg.fds + i; 
		arg.pPollItems[i].pPoll = &arg; 

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn; // 预处理
		struct epoll_event &ev = arg.pPollItems[i].stEvent; 

		if( fds[i].fd > -1 )  // fd有效
		{
			ev.data.ptr = arg.pPollItems + i; 
			ev.events = PollEvent2Epoll( fds[i].events ); 

			co_epoll_ctl( epfd, EPOLL_CTL_ADD,  fds[i].fd,  &ev );   // 将该fd关联的事件, 添加到epoll树中监视
		}
	}

	//3. add timeout  因为timeout一定不为0, 因此还要注册超时事件

	unsigned long long now = GetTickMS(); 
	arg.ullExpireTime = now + timeout; 
	int ret = AddTimeout( ctx->pTimeout, &arg, now ); 
	int iRaiseCnt = 0; 


	// 4. 添加完上面的2个事件(读写事件\超时事件)后, 此时切走协程? 为什么?
	//    答：
	//      (1) 因为已经将事件注册到epoll上了, 把事件托管给epoll管理
	//      (2) 当epoll事件触发后, 将执行 OnPollProcessEvent 函数, 切换到该事件绑定的协程函数co->pfn
	//          执行完协程函数co->pfn后, 将会切回
	co_yield_env( co_get_curr_thread_env() );   
	iRaiseCnt = arg.iRaiseCnt; 


	// 5. 事件执行完毕后, 将从epoll中摘除(超时事件, 读写事件)
    {
		//clear epoll status and memory
		RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>( &arg ); // 从epoll中摘除该超时事件
		for(nfds_t i = 0; i < nfds; i++)
		{
			int fd = fds[i].fd; 
			if( fd > -1 )
			{
				co_epoll_ctl( epfd, EPOLL_CTL_DEL, fd, &arg.pPollItems[i].stEvent );  // 从epoll中摘除该读写事件
			}
			fds[i].revents = arg.fds[i].revents; 
		}


		if( arg.pPollItems != arr )
		{
			free( arg.pPollItems ); 
			arg.pPollItems = NULL; 
		}

		free(arg.fds); 
		free(&arg); 
	}

	return iRaiseCnt; 
}

int	co_poll( stCoEpoll_t *ctx, struct pollfd fds[],  nfds_t nfds,  int timeout_ms )
{
	return co_poll_inner(ctx,  fds,  nfds,  timeout_ms,  NULL); 
}

void SetEpoll( stCoRoutineEnv_t *env, stCoEpoll_t *ev )
{
	env->pEpoll = ev; 
}
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env(); 
	}
	return co_get_curr_thread_env()->pEpoll; 
}

/*  */
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co; 
	void *value; 

	enum 
	{
		size = 1024
	}; 
}; 
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo(); 
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key ); 
	}
	return co->aSpec[ key ].value; 
}
int co_setspecific(pthread_key_t key,  const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo(); 
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key, value ); 
	}
	co->aSpec[ key ].value = (void*)value; 
	return 0; 
}


// hook开启表示设为0，表示关闭hook
void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo(); 
	if( co )
	{
		co->cEnableSysHook = 0; 
	}
}

// 标记是否开启hook
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo(); 
	return ( co && co->cEnableSysHook );   // 有正在运行的协程co && co钩子标记位开启
}

// 获取正在运行的协程co
stCoRoutine_t *co_self()
{
	return GetCurrThreadCo(); 
}

//co cond 协程条件变量很简单, 它就是一个双向链表
struct stCoCond_t; 

// 条件变量中等待的元素, 一般只有一个
struct stCoCondItem_t {
	// struct list_head 串成双向链表
	stCoCondItem_t *pPrev; 
	stCoCondItem_t *pNext; 

	// 所属的链表头
	stCoCond_t *pLink; 

	// 超时时间
	stTimeoutItem_t timeout; 
}; 

// 条件变量结构体
struct stCoCond_t
{
	stCoCondItem_t *head; 
	stCoCondItem_t *tail; 
}; 

static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg; 
	co_resume( co ); 
}

// 弹出并返回条件变量链表的链表头节点
stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
	stCoCondItem_t *p = link->head; 
	if( p )
	{
		PopHead<stCoCondItem_t, stCoCond_t>( link ); 
	}
	return p; 
}

// 唤醒第一个等待在条件变量上的事件
int co_cond_signal( stCoCond_t *si )
{
	stCoCondItem_t * sp = co_cond_pop( si ); 
	if( !sp ) 
	{
		return 0; 
	}
	RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>( &sp->timeout );  // 从超时队列中摘除
	AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList, &sp->timeout );   // 添加到激活链表

	return 0; 
}

// 唤醒所有等待在条件变量上的事件
int co_cond_broadcast( stCoCond_t *si )
{
	for(; ; )
	{
		stCoCondItem_t * sp = co_cond_pop( si ); 
		if( !sp ) return 0; 

		RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>( &sp->timeout ); // 从超时队列中摘除
		AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList, &sp->timeout );  // 添加到激活链表
	}

	return 0; 
}

/* 在条件变量上等待
 *  @param  [in]  link  条件变量, 外部定义env->cond   
 *  @param  [in]  ms    等待超时时间
 */
int co_cond_timedwait( stCoCond_t *link, int ms )
{
	stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1,  sizeof(stCoCondItem_t)); 
	psi->timeout.pArg = GetCurrThreadCo(); 
	psi->timeout.pfnProcess = OnSignalProcessEvent; 

	if( ms > 0 )
	{
		unsigned long long now = GetTickMS(); 
		psi->timeout.ullExpireTime = now + ms; 

		// 事件添加到等待链表
		int ret = AddTimeout( co_get_curr_thread_env()->pEpoll->pTimeout, &psi->timeout, now ); 
		if( ret != 0 )
		{
			free(psi); 
			return ret; 
		}
	}
	AddTail( link,  psi);   // 事件添加到等待条件变量链表中

	co_yield_ct(); // 切走协程, 当条件变量事件触发时, 切回

	RemoveFromLink<stCoCondItem_t, stCoCond_t>( psi );  // 切回后, 将该事件从条件变量链表中删除
	free(psi); 

	return 0; 
}
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t*)calloc( 1, sizeof(stCoCond_t) ); 
}
int co_cond_free( stCoCond_t * cc )
{
	free( cc ); 
	return 0; 
}



