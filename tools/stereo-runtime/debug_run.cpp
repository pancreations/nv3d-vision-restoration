// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded debugger for a process this tool creates. Does not attach to or stop
// unrelated games. Logs second-chance failures without changing their outcome.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
int wmain(int argc,wchar_t** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    wchar_t diagnostics[2]{};const bool traceLoads=GetEnvironmentVariableW(L"VISION_DEBUG_LOADS",diagnostics,2)>0;
    const bool traceExit=GetEnvironmentVariableW(L"VISION_DEBUG_EXIT_TRACE",diagnostics,2)>0;
    if(argc<2){std::puts("Usage: vision_debug_run program [args]");return 2;}
    std::wstring command=L"\""+std::wstring(argv[1])+L"\"";
    for(int i=2;i<argc;++i)command+=L" \""+std::wstring(argv[i])+L"\"";
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    auto dir=std::filesystem::absolute(argv[1]).parent_path();
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS,nullptr,dir.c_str(),&si,&pi)){std::printf("CreateProcess error=%lu\n",GetLastError());return 2;}
    CloseHandle(pi.hThread);DWORD result=1;bool symbols=false;const auto start=GetTickCount64();
    std::vector<DWORD> threads;uintptr_t exitAddress=0;
    auto armExit=[&](DWORD id){
        if(!exitAddress)return;
        HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,id);
        CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
        if(thread&&GetThreadContext(thread,&c)){c.Dr0=exitAddress;c.Dr7=(c.Dr7&~DWORD64(0xf0003))|1;SetThreadContext(thread,&c);}
        if(thread)CloseHandle(thread);
    };
    for(;;){if(GetTickCount64()-start>60000)TerminateProcess(pi.hProcess,10);DEBUG_EVENT e{};if(!WaitForDebugEvent(&e,100)){
        if(GetTickCount64()-start>60000){std::puts("FAIL: child exceeded 60 seconds");TerminateProcess(pi.hProcess,10);}continue;
    }
        DWORD disposition=DBG_CONTINUE;
        if(traceExit){
            if(e.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT||e.dwDebugEventCode==CREATE_THREAD_DEBUG_EVENT){threads.push_back(e.dwThreadId);armExit(e.dwThreadId);}
            if(e.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT&&e.u.LoadDll.hFile){
                wchar_t path[1024]{};
                if(GetFinalPathNameByHandleW(e.u.LoadDll.hFile,path,1024,0)&&_wcsicmp(std::filesystem::path(path).filename().c_str(),L"ntdll.dll")==0){
                    const auto local=GetModuleHandleW(L"ntdll.dll");const auto entry=GetProcAddress(local,"RtlExitUserProcess");
                    exitAddress=reinterpret_cast<uintptr_t>(e.u.LoadDll.lpBaseOfDll)+reinterpret_cast<uintptr_t>(entry)-reinterpret_cast<uintptr_t>(local);
                    for(auto id:threads)armExit(id);
                }
            }
        }
        if(e.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT){if(e.u.CreateProcessInfo.hFile)CloseHandle(e.u.CreateProcessInfo.hFile);CloseHandle(e.u.CreateProcessInfo.hThread);CloseHandle(e.u.CreateProcessInfo.hProcess);}
        if(e.dwDebugEventCode==CREATE_THREAD_DEBUG_EVENT)CloseHandle(e.u.CreateThread.hThread);
        if(e.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT&&e.u.LoadDll.hFile){if(traceLoads){wchar_t path[1024]{};if(GetFinalPathNameByHandleW(e.u.LoadDll.hFile,path,1024,0))std::wprintf(L"LOAD: %ls\n",path);}CloseHandle(e.u.LoadDll.hFile);}
        if(traceLoads&&e.dwDebugEventCode==OUTPUT_DEBUG_STRING_EVENT){auto& info=e.u.DebugString;char bytes[4096]{};SIZE_T count=0;
            const SIZE_T length=std::min(SIZE_T(info.nDebugStringLength)*(info.fUnicode?2:1),sizeof(bytes)-2);
            if(ReadProcessMemory(pi.hProcess,info.lpDebugStringData,bytes,length,&count)){
                if(info.fUnicode)std::wprintf(L"DEBUG: %ls\n",reinterpret_cast<const wchar_t*>(bytes));else std::printf("DEBUG: %s\n",bytes);
            }
        }
        if(e.dwDebugEventCode==EXCEPTION_DEBUG_EVENT){auto& ex=e.u.Exception;auto& record=ex.ExceptionRecord;
            if(record.ExceptionCode!=EXCEPTION_BREAKPOINT){disposition=DBG_EXCEPTION_NOT_HANDLED;
                const bool exiting=traceExit&&record.ExceptionCode==EXCEPTION_SINGLE_STEP&&reinterpret_cast<uintptr_t>(record.ExceptionAddress)==exitAddress;
                if(exiting)disposition=DBG_CONTINUE;
                if(!ex.dwFirstChance||exiting){
                    if(exiting)std::puts("PROCESS EXIT CALL STACK (not an unhandled crash):");
                    std::printf("EXCEPTION exception 0x%08lx at %p thread=%lu\n",record.ExceptionCode,record.ExceptionAddress,e.dwThreadId);
                    if(!symbols){symbols=SymInitialize(pi.hProcess,nullptr,TRUE)!=FALSE;}else SymRefreshModuleList(pi.hProcess);
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,e.dwThreadId);CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
                    if(thread&&GetThreadContext(thread,&context)){
                        STACKFRAME64 frame{};
#ifdef _WIN64
                        frame.AddrPC.Offset=context.Rip;frame.AddrStack.Offset=context.Rsp;frame.AddrFrame.Offset=context.Rbp;DWORD machine=IMAGE_FILE_MACHINE_AMD64;
#else
                        frame.AddrPC.Offset=context.Eip;frame.AddrStack.Offset=context.Esp;frame.AddrFrame.Offset=context.Ebp;DWORD machine=IMAGE_FILE_MACHINE_I386;
#endif
                        frame.AddrPC.Mode=frame.AddrStack.Mode=frame.AddrFrame.Mode=AddrModeFlat;
                        for(int n=0;n<24&&frame.AddrPC.Offset;++n){IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);
                            if(SymGetModuleInfo64(pi.hProcess,frame.AddrPC.Offset,&module))std::printf("  %s + 0x%llx\n",module.ModuleName,frame.AddrPC.Offset-module.BaseOfImage);
                            else std::printf("  0x%llx\n",frame.AddrPC.Offset);
                            if(!StackWalk64(machine,pi.hProcess,thread,&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
                        }
                    }
                    if(exiting&&thread){CONTEXT resume{};resume.ContextFlags=CONTEXT_DEBUG_REGISTERS;if(GetThreadContext(thread,&resume)){resume.Dr7&=~DWORD64(3);resume.Dr6=0;SetThreadContext(thread,&resume);}}
                    if(thread)CloseHandle(thread);
                }
            }
        }
        if(e.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT){result=e.u.ExitProcess.dwExitCode;std::printf("Child exit: %lu (0x%08lx)\n",result,result);ContinueDebugEvent(e.dwProcessId,e.dwThreadId,DBG_CONTINUE);break;}
        ContinueDebugEvent(e.dwProcessId,e.dwThreadId,disposition);
    }
    if(symbols)SymCleanup(pi.hProcess);CloseHandle(pi.hProcess);return int(result);
}
