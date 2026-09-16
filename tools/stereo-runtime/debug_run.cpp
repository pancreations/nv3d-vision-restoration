// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded debugger for a process this tool creates. Does not attach to or stop
// unrelated games. Logs second-chance failures without changing their outcome.
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <filesystem>
#include <string>
int wmain(int argc,wchar_t** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<2){std::puts("Usage: vision_debug_run program [args]");return 2;}
    std::wstring command=L"\""+std::wstring(argv[1])+L"\"";
    for(int i=2;i<argc;++i)command+=L" \""+std::wstring(argv[i])+L"\"";
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    auto dir=std::filesystem::absolute(argv[1]).parent_path();
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS,nullptr,dir.c_str(),&si,&pi)){std::printf("CreateProcess error=%lu\n",GetLastError());return 2;}
    CloseHandle(pi.hThread);DWORD result=1;bool symbols=false;const auto start=GetTickCount64();
    for(;;){DEBUG_EVENT e{};if(!WaitForDebugEvent(&e,100)){
        if(GetTickCount64()-start>60000){std::puts("FAIL: child exceeded 60 seconds");TerminateProcess(pi.hProcess,10);}continue;
    }
        DWORD disposition=DBG_CONTINUE;
        if(e.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT){if(e.u.CreateProcessInfo.hFile)CloseHandle(e.u.CreateProcessInfo.hFile);CloseHandle(e.u.CreateProcessInfo.hThread);CloseHandle(e.u.CreateProcessInfo.hProcess);}
        if(e.dwDebugEventCode==CREATE_THREAD_DEBUG_EVENT)CloseHandle(e.u.CreateThread.hThread);
        if(e.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT&&e.u.LoadDll.hFile)CloseHandle(e.u.LoadDll.hFile);
        if(e.dwDebugEventCode==EXCEPTION_DEBUG_EVENT){auto& ex=e.u.Exception;auto& record=ex.ExceptionRecord;
            if(record.ExceptionCode!=EXCEPTION_BREAKPOINT){disposition=DBG_EXCEPTION_NOT_HANDLED;
                if(!ex.dwFirstChance){
                    std::printf("UNHANDLED exception 0x%08lx at %p thread=%lu\n",record.ExceptionCode,record.ExceptionAddress,e.dwThreadId);
                    if(!symbols){symbols=SymInitialize(pi.hProcess,nullptr,TRUE)!=FALSE;}else SymRefreshModuleList(pi.hProcess);
                    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,e.dwThreadId);CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
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
                    }if(thread)CloseHandle(thread);
                }
            }
        }
        if(e.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT){result=e.u.ExitProcess.dwExitCode;std::printf("Child exit: %lu (0x%08lx)\n",result,result);ContinueDebugEvent(e.dwProcessId,e.dwThreadId,DBG_CONTINUE);break;}
        ContinueDebugEvent(e.dwProcessId,e.dwThreadId,disposition);
    }
    if(symbols)SymCleanup(pi.hProcess);CloseHandle(pi.hProcess);return int(result);
}
