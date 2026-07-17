/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     What is this file about?
 *
 * Revision history:
 *     xxxx-xx-xx, author, first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */


# pragma once

# include <dsn/service_api_cpp.h>
# include <dsn/cpp/test_output_utils.h>
# include <dsn/tool-api/task.h>
# include <dsn/tool-api/task_worker.h>

using namespace ::dsn;

#ifndef TEST_PORT_BEGIN
#define TEST_PORT_BEGIN 20201
#define TEST_PORT_END 20203
#endif

DEFINE_THREAD_POOL_CODE(THREAD_POOL_TEST_SERVER)

DEFINE_TASK_CODE_RPC(RPC_TEST_HASH, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_HASH1, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_HASH2, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_HASH3, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_HASH4, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_HASH5, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_HASH6, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)
DEFINE_TASK_CODE_RPC(RPC_TEST_STRING_COMMAND, TASK_PRIORITY_COMMON, THREAD_POOL_TEST_SERVER)

DEFINE_TASK_CODE_AIO(LPC_AIO_TEST, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)
DEFINE_TASK_CODE(LPC_TEST_HASH, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)
DEFINE_TASK_CODE_AIO(LPC_AIO_TEST_READ, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)
DEFINE_TASK_CODE_AIO(LPC_AIO_TEST_WRITE, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)
DEFINE_TASK_CODE_AIO(LPC_AIO_TEST_NFS, TASK_PRIORITY_COMMON, THREAD_POOL_DEFAULT)
