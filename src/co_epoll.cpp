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

#include "co_epoll.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

// 监听注册在epoll树上的事件, 返回值: 就绪事件的总数
int	co_epoll_wait( int epfd, struct co_epoll_res *events, int maxevents, int timeout )
{
	return epoll_wait( epfd,  events->events,  maxevents,  timeout );
}


/*
 * @brief  事件管理: 插入, 删除, 修改
 * @param  [in] op: EPOLL_CTL_ADD, EPOLL_CTL_DEL, EPOLL_CTL_MOD
 */
int	co_epoll_ctl( int epfd, int op, int fd, struct epoll_event * ev )
{
	return epoll_ctl( epfd, op, fd, ev ); 
}

int	co_epoll_create( int size )
{
	return epoll_create( size );
}

// 创建数组, 保存epoll_wait返回的结果
struct co_epoll_res *co_epoll_res_alloc( int n )
{
	struct co_epoll_res * ptr = 
		(struct co_epoll_res *)malloc( sizeof( struct co_epoll_res ) );

	ptr->size = n;
	ptr->events = (struct epoll_event*)calloc( 1, n * sizeof( struct epoll_event ) );

	return ptr;

}
void co_epoll_res_free( struct co_epoll_res * ptr )
{
	if( !ptr ) return;
	if( ptr->events ) free( ptr->events );
	free( ptr );
}
