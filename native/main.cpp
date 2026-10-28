#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include <sstream>
#include <regex>
#include <functional>
#include <thread>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <fstream>

using S = std::wstring;
struct Failure { S text; };
void require(bool ok, const S& message) { if (!ok) throw Failure{message}; }
S trim(S s) { auto a=s.find_first_not_of(L" \t\r\n"); if(a==S::npos)return L""; return s.substr(a,s.find_last_not_of(L" \t\r\n")-a+1); }
S text(HWND h) { int n=GetWindowTextLengthW(h); S s(n+1,0); GetWindowTextW(h,&s[0],n+1); s.resize(n); return s; }
S quote(const S& s) {
    S out=L"\""; size_t slashes=0;
    for(wchar_t c:s) { if(c==L'\\') { ++slashes; continue; }
        out.append(slashes*(c==L'"'?2:1),L'\\'); slashes=0;
        if(c==L'"')out+=L'\\'; out+=c;
    }
    out.append(slashes*2,L'\\'); return out+L'"';
}
S decode(const std::string& b) {
    if(b.empty())return L"";
    if(b.find('\0')!=std::string::npos) {
        S out; for(size_t i=0;i+1<b.size();i+=2)out+=wchar_t((unsigned char)b[i]|((unsigned char)b[i+1]<<8));
        if(!out.empty()&&out[0]==0xfeff)out.erase(0,1); return trim(out);
    }
    UINT cp=CP_UTF8; int n=MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,b.data(),(int)b.size(),nullptr,0);
    if(!n) {cp=GetOEMCP(); n=MultiByteToWideChar(cp,0,b.data(),(int)b.size(),nullptr,0);}
    S out(n,0); MultiByteToWideChar(cp,0,b.data(),(int)b.size(),&out[0],n); return trim(out);
}
struct Handle { HANDLE h=nullptr; ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);} };
S executable(const S& name) {
    wchar_t found[32768]; DWORD n=SearchPathW(nullptr,name.c_str(),nullptr,32768,found,nullptr);
    if(n&&n<32768)return found;
    if(name==L"usbipd.exe") {
        wchar_t pf[32768]; if(GetEnvironmentVariableW(L"ProgramW6432",pf,32768)) {
            S path=S(pf)+L"\\usbipd-win\\usbipd.exe";
            if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return path;
        }
    }
    throw Failure{L"未找到 "+name+L"。USB 需要先安装 usbipd-win。"};
}
S run(const S& name, const std::vector<S>& args, DWORD timeout=25000) {
    S exe=executable(name), cmd=quote(exe);
    // WSL's command parser treats unnecessarily quoted switches as a Linux
    // command on some releases. Quote only arguments requiring CRT escaping.
    for(auto& arg:args)cmd+=L" "+(!arg.empty()&&arg.find_first_of(L" \t\n\v\"")==S::npos?arg:quote(arg));
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; Handle read,write,input;
    require(CreatePipe(&read.h,&write.h,&sa,0)!=0,L"无法创建命令输出管道");
    SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=si.hStdError=write.h; si.hStdInput=input.h;
    PROCESS_INFORMATION pi{};
    require(CreateProcessW(exe.c_str(),&cmd[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=0,L"无法启动 "+name);
    Handle process{pi.hProcess},thread{pi.hThread}; CloseHandle(write.h); write.h=nullptr;
    std::string output; ULONGLONG start=GetTickCount64(); bool ended=false;
    while(true) {
        DWORD available=0;
        if(PeekNamedPipe(read.h,nullptr,0,nullptr,&available,nullptr)&&available) {
            char buffer[8192]; DWORD count=0;
            if(ReadFile(read.h,buffer,std::min<DWORD>(available,sizeof(buffer)),&count,nullptr)) {
                if(output.size()<4*1024*1024)output.append(buffer,count);
            }
        } else if(ended)break; else Sleep(15);
        ended=WaitForSingleObject(process.h,0)==WAIT_OBJECT_0;
        if(!ended&&GetTickCount64()-start>timeout) { TerminateProcess(process.h,1); WaitForSingleObject(process.h,2000); throw Failure{name+L" 超时。请检查实际状态后再重试。"}; }
    }
    DWORD code=1; GetExitCodeProcess(process.h,&code);
    S result=decode(output); require(code==0,name+L" 执行失败：\r\n"+result);
    return result;
}
struct Distro { S name,state; int version=0; bool isDefault=false; };
std::vector<Distro> parseDistros(const S& output) {
    std::vector<Distro> all; std::wistringstream lines(output); S line;
    std::wregex re(LR"(^\s*(\*?)\s*(.+?)\s{2,}(\S+)\s+([12])\s*$)"); std::wsmatch m;
    while(std::getline(lines,line))if(std::regex_match(line,m,re))all.push_back({trim(m[2]),m[3],std::stoi(m[4]),m[1]==L"*"});
    return all;
}
void ipv4(const S& s) { IN_ADDR addr{}; require(InetPtonW(AF_INET,s.c_str(),&addr)==1,L"请输入有效 IPv4 地址："+s); }
int port(const S& s) { require(std::regex_match(s,std::wregex(L"[0-9]{1,5}")),L"端口必须为 1–65535"); int p=std::stoi(s); require(p>0&&p<=65535,L"端口必须为 1–65535"); return p; }
void busid(const S& s) {require(std::regex_match(s,std::wregex(LR"(\d+-\d+(\.\d+)*)")),L"BUSID 格式应类似 1-2");}
bool admin() { BOOL yes=FALSE; SID_IDENTIFIER_AUTHORITY auth=SECURITY_NT_AUTHORITY; PSID sid=nullptr;
    if(AllocateAndInitializeSid(&auth,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&yes);FreeSid(sid);} return yes!=FALSE; }
S wsl(const S& distro,const std::vector<S>& command) { std::vector<S> args{L"-d",distro,L"--"}; args.insert(args.end(),command.begin(),command.end()); return run(L"wsl.exe",args); }
void requireWsl2(const S& distro) { auto entries=parseDistros(run(L"wsl.exe",{L"--list",L"--verbose"}));
    for(auto& d:entries)if(d.name==distro){require(d.version==2,L"此功能需要 WSL 2。请先选择 WSL 2 发行版。");return;} throw Failure{L"发行版不存在，请手动检测。"}; }

struct Rule { S id,distro,listen=L"127.0.0.1",target=L"auto",resolved; int from=8080,to=80; bool reverse=false,owned=false; };
S dataPath,iniPath;
std::vector<Rule> rules;
S readIni(const S& section,const wchar_t* key,const wchar_t* fallback=L"") { wchar_t buffer[4096]; GetPrivateProfileStringW(section.c_str(),key,fallback,buffer,4096,iniPath.c_str()); return buffer; }
void writeIni(const S& section,const wchar_t* key,const S& value,const S& path) {require(WritePrivateProfileStringW(section.c_str(),key,value.c_str(),path.c_str())!=0,L"保存规则失败");}
void validate(const Rule& r) { require(std::regex_match(r.id,std::wregex(L"[a-f0-9]{32}")),L"规则 ID 无效"); ipv4(r.listen);if(r.target!=L"auto")ipv4(r.target); port(std::to_wstring(r.from));port(std::to_wstring(r.to)); require(!r.distro.empty()&&r.distro[0]!=L'-',L"请选择发行版"); }
void save() {
    S tmp=iniPath+L".tmp"; DeleteFileW(tmp.c_str());
    // Unicode INI requires a BOM for the Win32 profile API.
    Handle file{CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr)}; require(file.h!=INVALID_HANDLE_VALUE,L"无法写入配置");
    WORD bom=0xfeff;DWORD n; WriteFile(file.h,&bom,2,&n,nullptr); CloseHandle(file.h);file.h=nullptr;
    writeIni(L"meta",L"count",std::to_wstring(rules.size()),tmp);
    for(size_t i=0;i<rules.size();++i) {const auto& r=rules[i]; S k=L"rule"+std::to_wstring(i);
        writeIni(k,L"id",r.id,tmp);writeIni(k,L"distro",r.distro,tmp);writeIni(k,L"listen",r.listen,tmp);writeIni(k,L"target",r.target,tmp);writeIni(k,L"resolved",r.resolved,tmp);
        writeIni(k,L"from",std::to_wstring(r.from),tmp);writeIni(k,L"to",std::to_wstring(r.to),tmp);writeIni(k,L"reverse",r.reverse?L"1":L"0",tmp);writeIni(k,L"owned",r.owned?L"1":L"0",tmp);
    }
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,tmp.c_str());
    require(MoveFileExW(tmp.c_str(),iniPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,L"无法替换配置文件");
}
void load() {
    int count=GetPrivateProfileIntW(L"meta",L"count",0,iniPath.c_str()); require(count>=0&&count<=1000,L"规则文件损坏");
    for(int i=0;i<count;i++){ S k=L"rule"+std::to_wstring(i);Rule r;
        r.id=readIni(k,L"id");r.distro=readIni(k,L"distro");r.listen=readIni(k,L"listen");r.target=readIni(k,L"target");r.resolved=readIni(k,L"resolved");
        r.from=port(readIni(k,L"from"));r.to=port(readIni(k,L"to"));r.reverse=readIni(k,L"reverse")==L"1";r.owned=readIni(k,L"owned")==L"1";validate(r);rules.push_back(r);
    }
}
S newId() { GUID guid;require(SUCCEEDED(CoCreateGuid(&guid)),L"无法生成规则 ID");wchar_t s[40];StringFromGUID2(guid,s,40);S id;for(wchar_t c:S(s))if(iswxdigit(c))id+=towlower(c);return id; }
S resolve(const Rule& r) {
    if(r.target!=L"auto")return r.target;
    if(!r.reverse) {std::wistringstream words(wsl(r.distro,{L"hostname",L"-I"}));S ip;while(words>>ip){try{ipv4(ip);return ip;}catch(Failure&) {}} throw Failure{L"未找到 WSL IPv4"};}
    S mode;try{mode=wsl(r.distro,{L"wslinfo",L"--networking-mode"});}catch(Failure&){}
    if(mode==L"mirrored")return L"127.0.0.1";
    if(mode==L"nat") {S route=wsl(r.distro,{L"ip",L"-4",L"route",L"show",L"default"});std::wsmatch m;if(std::regex_search(route,m,std::wregex(LR"(\bvia\s+(\d+\.\d+\.\d+\.\d+))"))){ipv4(m[1]);return m[1];}}
    throw Failure{L"无法自动定位 Windows 地址（旧 WSL 或其他网络模式），请手动填写目标 IPv4。"};
}
std::vector<S> netRule(const Rule& r,const S& verb) {std::vector<S> a{L"interface",L"portproxy",verb,L"v4tov4",L"listenaddress="+r.listen,L"listenport="+std::to_wstring(r.from)};
    if(verb==L"add"){a.push_back(L"connectaddress="+r.resolved);a.push_back(L"connectport="+std::to_wstring(r.to));}return a;}
S existingTarget(const Rule& r) {std::wistringstream lines(run(L"netsh.exe",{L"interface",L"portproxy",L"show",L"v4tov4"}));S line;
    while(std::getline(lines,line)){std::wistringstream fields(line);S a,b,c,d;fields>>a>>b>>c>>d;if(a==r.listen&&b==std::to_wstring(r.from))return c+L":"+d;}return L"";}
Rule startRule(Rule r) {
    validate(r);requireWsl2(r.distro);r.resolved=resolve(r);ipv4(r.resolved);
    if(!r.reverse){require(admin(),L"请右键 EXE，以管理员身份运行。");require(existingTarget(r).empty(),L"此地址端口已存在 portproxy 规则，未覆盖。请先核对实际状态。");run(L"netsh.exe",netRule(r,L"add"));}
    else {S pid=L"/tmp/wsl-tunnel-native-"+r.id+L".pid",log=L"/tmp/wsl-tunnel-native-"+r.id+L".log";
        S cmd=L"command -v socat >/dev/null || { echo 'Install socat in this distro'; exit 1; }; test ! -f "+pid+L" || { echo 'Stop existing rule first'; exit 1; }; nohup socat TCP4-LISTEN:"+std::to_wstring(r.from)+L",bind="+r.listen+L",reuseaddr,fork TCP4:"+r.resolved+L":"+std::to_wstring(r.to)+L" </dev/null >"+log+L" 2>&1 & p=$!; echo $p >"+pid+L"; sleep 0.3; kill -0 $p 2>/dev/null || { cat "+log+L"; rm -f "+pid+L"; exit 1; }";
        wsl(r.distro,{L"sh",L"-c",cmd});
    }r.owned=true;return r;
}
Rule stopRule(Rule r) {
    validate(r);require(r.owned,L"此规则没有本程序的启动记录。");
    if(!r.reverse){require(admin(),L"请以管理员身份运行。");S found=existingTarget(r);require(found.empty()||found==r.resolved+L":"+std::to_wstring(r.to),L"规则已被其他工具修改，未删除。");if(!found.empty())run(L"netsh.exe",netRule(r,L"delete"));}
    else {S pid=L"/tmp/wsl-tunnel-native-"+r.id+L".pid";
        S cmd=L"if test -f "+pid+L"; then p=$(cat "+pid+L"); case $p in ''|*[!0-9]*) exit 1;; esac; if test -r /proc/$p/cmdline; then tr '\000' ' ' </proc/$p/cmdline | grep -F 'TCP4-LISTEN:"+std::to_wstring(r.from)+L",bind="+r.listen+L",' >/dev/null || { echo 'Process identity changed'; exit 1; }; kill $p || exit 1; fi; rm -f "+pid+L"; fi";
        wsl(r.distro,{L"sh",L"-c",cmd});
    }r.owned=false;return r;
}

HWND window,combo,statusLabel,tabs,logBox,ruleList,usbOutput,direction,listenEdit,fromEdit,targetEdit,toEdit,busEdit;
HFONT font=nullptr,titleFont=nullptr; UINT dpi=96; bool busy=false,smoke=false; int page=0;
bool smokeFailed=false;
std::vector<HWND> networkControls,usbControls,allControls,buttons;
std::vector<Distro> distros;
constexpr UINT WM_RESULT=WM_APP+1;
enum {Refresh=100,Distros,Tab,Add,Start,Stop,Remove,State,UsbList,Bind,Attach,Detach,Unbind};
struct Result {std::function<void()> apply;S error;};
void log(const S& message) {S entry=message+L"\r\n";SendMessageW(logBox,EM_SETSEL,-1,-1);SendMessageW(logBox,EM_REPLACESEL,FALSE,(LPARAM)entry.c_str());}
void enable() {for(auto h:buttons)EnableWindow(h,!busy);EnableWindow(combo,!busy);}
void job(std::function<std::function<void()>()> work) {
    if(busy)return;busy=true;enable();SetWindowTextW(statusLabel,L"正在处理…");
    std::thread([work](){auto r=new Result;try{r->apply=work();}catch(const Failure& e){r->error=e.text;}catch(...){r->error=L"操作失败。";}PostMessageW(window,WM_RESULT,0,(LPARAM)r);}).detach();
}
void versionStatus() {
    int i=(int)SendMessageW(combo,CB_GETCURSEL,0,0);S s=L"未选择发行版";
    if(i>=0&&i<(int)distros.size()){auto& d=distros[i];s=d.name+L"  ·  WSL "+std::to_wstring(d.version)+L"  ·  "+d.state;if(d.version==1)s+=L"  ·  当前功能需要 WSL 2";}
    SetWindowTextW(statusLabel,s.c_str());
}
S selectedDistro(){int i=(int)SendMessageW(combo,CB_GETCURSEL,0,0);require(i>=0&&i<(int)distros.size(),L"请先检测并选择 WSL 发行版。");return distros[i].name;}
void discover() {
    S previous=text(combo); job([previous](){auto found=parseDistros(run(L"wsl.exe",{L"--list",L"--verbose"}));return [found,previous](){distros=found;SendMessageW(combo,CB_RESETCONTENT,0,0);int selected=0;
        for(size_t i=0;i<distros.size();++i){SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)distros[i].name.c_str());if(distros[i].isDefault)selected=(int)i;}
        for(size_t i=0;i<distros.size();++i)if(distros[i].name==previous)selected=(int)i;
        if(!distros.empty())SendMessageW(combo,CB_SETCURSEL,selected,0);
        log(L"检测到 "+std::to_wstring(distros.size())+L" 个发行版。");};});
}
void refreshRules() {
    ListView_DeleteAllItems(ruleList);
    for(size_t i=0;i<rules.size();++i){auto& r=rules[i];S fields[]={r.reverse?L"WSL → Windows":L"Windows → WSL",r.listen+L":"+std::to_wstring(r.from),r.target+L":"+std::to_wstring(r.to),r.distro,r.owned?L"有启动记录":L"未启动"};
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=(int)i;item.pszText=&fields[0][0];ListView_InsertItem(ruleList,&item);
        for(int j=1;j<5;++j)ListView_SetItemText(ruleList,(int)i,j,&fields[j][0]);
    }
}
int selectedRule(){int i=ListView_GetNextItem(ruleList,-1,LVNI_SELECTED);require(i>=0&&i<(int)rules.size(),L"请先选择规则。");return i;}
void action(int id) {
    if(busy)return;
    try {
        if(id==Refresh){discover();return;}
        if(id==Add){Rule r;r.id=newId();r.distro=selectedDistro();r.listen=trim(text(listenEdit));r.target=trim(text(targetEdit));r.from=port(text(fromEdit));r.to=port(text(toEdit));r.reverse=SendMessageW(direction,CB_GETCURSEL,0,0)==1;validate(r);
            for(auto& e:rules)require(!(e.listen==r.listen&&e.from==r.from&&e.reverse==r.reverse&&(!r.reverse||e.distro==r.distro)),L"已有相同监听规则。");
            rules.push_back(r);try{save();}catch(...){rules.pop_back();throw;}refreshRules();log(L"规则已保存，选中后启动。");return;}
        if(id==Remove){int i=selectedRule();require(!rules[i].owned,L"请先停止此规则。");auto old=rules;rules.erase(rules.begin()+i);try{save();}catch(...){rules=old;throw;}refreshRules();return;}
        if(id==Start||id==Stop){int i=selectedRule();Rule r=rules[i];job([r,i,id](){Rule next=id==Start?startRule(r):stopRule(r);return [next,i,id](){rules[i]=next;save();refreshRules();log(id==Start?L"已启动。关闭窗口不会停止转发。":L"监听已停止；已有连接可能持续到客户端断开。");};});return;}
        if(id==State){S d=selectedDistro();job([d](){S output=run(L"netsh.exe",{L"interface",L"portproxy",L"show",L"v4tov4"})+L"\r\n"+wsl(d,{L"ss",L"-ltnp"});return [output](){log(output);};});return;}
        if(id>=UsbList&&id<=Unbind){S bus=trim(text(busEdit)),d;if(id!=UsbList)busid(bus);if(id==Attach)d=selectedDistro();
            if(id==Bind||id==Unbind)require(admin(),L"首次共享/取消共享需要 Windows 管理员权限。请右键 EXE 以管理员身份运行。");
            job([id,bus,d](){S output;if(id==UsbList)output=run(L"usbipd.exe",{L"list"});else {
                S verb=id==Bind?L"bind":id==Attach?L"attach":id==Detach?L"detach":L"unbind";
                std::vector<S> args{verb};if(id==Attach){requireWsl2(d);require(run(L"usbipd.exe",{L"attach",L"--help"}).find(L"--wsl")!=S::npos,L"请升级 usbipd-win。");wsl(d,{L"true"});args.push_back(L"--wsl");}args.push_back(L"--busid");args.push_back(bus);output=run(L"usbipd.exe",args);}
                return [output,id](){if(id==UsbList)SetWindowTextW(usbOutput,output.c_str());else log(output.empty()?L"USB 操作完成，请刷新设备查看状态。":output);};});
        }
    } catch(const Failure& e){log(e.text);MessageBoxW(window,e.text.c_str(),L"WSL Tunnel",MB_OK|MB_ICONINFORMATION);}
}
HWND control(const wchar_t* cls,const S& label,DWORD style,int id=0,int group=-1) {
    HWND h=CreateWindowExW(cls==S(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,label.c_str(),WS_CHILD|WS_VISIBLE|style,0,0,0,0,window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    allControls.push_back(h);if(group==0)networkControls.push_back(h);if(group==1)usbControls.push_back(h);if(cls==S(L"BUTTON"))buttons.push_back(h);
    SetWindowTheme(h,L"Explorer",nullptr);return h;
}
std::vector<HWND> netLabels;HWND heading,subtitle,versionTitle,netInfo,netHint,usbInfo,usbHint,busLabel;
std::vector<HWND> netButtons,usbButtons; HWND refreshButton;
int px(int v){return MulDiv(v,dpi,96);}
void move(HWND h,int x,int y,int w,int height){MoveWindow(h,px(x),px(y),px(std::max(1,w)),px(height),TRUE);}
void fonts() {
    HFONT old=font,oldTitle=titleFont;
    font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    titleFont=CreateFontW(-MulDiv(22,dpi,72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for(auto h:allControls)SendMessageW(h,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(heading,WM_SETFONT,(WPARAM)titleFont,TRUE);
    if(old)DeleteObject(old);if(oldTitle)DeleteObject(oldTitle);
}
void layout() {
    RECT rc;GetClientRect(window,&rc);int w=MulDiv(rc.right,96,dpi),h=MulDiv(rc.bottom,96,dpi),gap=12,inner=w-48;
    move(heading,24,14,w-48,36);move(subtitle,26,56,w-48,24);
    move(versionTitle,24,100,100,30);move(combo,128,96,w-320,300);move(refreshButton,w-172,96,148,32);move(statusLabel,128,134,w-152,30);
    move(tabs,24,176,w-48,36);move(logBox,24,h-110,w-48,90);
    move(netInfo,24,228,inner,28);move(direction,24,266,inner,250);
    int widths[]={inner*26/100,inner*16/100,inner*38/100,0};widths[3]=inner-widths[0]-widths[1]-widths[2]-3*gap;
    HWND edits[]={listenEdit,fromEdit,targetEdit,toEdit};int x=24;
    for(int i=0;i<4;++i){move(netLabels[i],x,312,widths[i],26);move(edits[i],x,344,widths[i],30);x+=widths[i]+gap;}
    move(netHint,24,386,inner,32);int bw=(inner-4*gap)/5;
    for(int i=0;i<5;++i)move(netButtons[i],24+i*(bw+gap),432,bw,36);
    move(ruleList,24,484,inner,std::max(90,h-614));
    int cols[]={19,23,23,23,12};for(int i=0;i<5;++i)ListView_SetColumnWidth(ruleList,i,px((inner-8)*cols[i]/100));
    move(usbInfo,24,228,inner,32);move(usbHint,24,270,inner,44);move(busLabel,24,328,120,24);move(busEdit,148,322,180,32);
    for(int i=0;i<5;++i)move(usbButtons[i],24+i*(bw+gap),376,bw,36);
    move(usbOutput,24,432,inner,std::max(150,h-562));
    for(auto c:networkControls)ShowWindow(c,page==0?SW_SHOW:SW_HIDE);for(auto c:usbControls)ShowWindow(c,page==1?SW_SHOW:SW_HIDE);
    InvalidateRect(window,nullptr,TRUE);
}
void create() {
    heading=control(L"STATIC",L"WSL Tunnel",0);subtitle=control(L"STATIC",L"原生 Windows  ·  端口转发 / USB 直通",0);
    versionTitle=control(L"STATIC",L"WSL 发行版",0);combo=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,Distros);
    refreshButton=control(L"BUTTON",L"手动检测",WS_TABSTOP,Refresh);statusLabel=control(L"STATIC",L"启动时自动检测…",0);
    tabs=control(WC_TABCONTROLW,L"",WS_TABSTOP,Tab);TCITEMW tab{};tab.mask=TCIF_TEXT;tab.pszText=(LPWSTR)L"  端口转发  ";TabCtrl_InsertItem(tabs,0,&tab);tab.pszText=(LPWSTR)L"  USB 直通  ";TabCtrl_InsertItem(tabs,1,&tab);
    netInfo=control(L"STATIC",L"在 Windows 与所选 WSL 发行版之间转发 TCP 连接。",0,0,0);
    direction=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,0,0);SendMessageW(direction,CB_ADDSTRING,0,(LPARAM)L"Windows → WSL");SendMessageW(direction,CB_ADDSTRING,0,(LPARAM)L"WSL → Windows");SendMessageW(direction,CB_SETCURSEL,0,0);
    for(auto label:{L"监听 IPv4",L"监听端口",L"目标 IPv4（auto 自动）",L"目标端口"})netLabels.push_back(control(L"STATIC",label,0,0,0));
    listenEdit=control(L"EDIT",L"127.0.0.1",ES_AUTOHSCROLL|WS_TABSTOP,0,0);fromEdit=control(L"EDIT",L"8080",ES_NUMBER|WS_TABSTOP,0,0);targetEdit=control(L"EDIT",L"auto",ES_AUTOHSCROLL|WS_TABSTOP,0,0);toEdit=control(L"EDIT",L"80",ES_NUMBER|WS_TABSTOP,0,0);
    netHint=control(L"STATIC",L"默认仅本机访问 · TCP/IPv4 · WSL → Windows 需要 socat · 退出窗口保留转发",0,0,0);
    const wchar_t* labels[]={L"保存新规则",L"启动",L"停止",L"删除",L"实际状态"};for(int i=0;i<5;++i)netButtons.push_back(control(L"BUTTON",labels[i],WS_TABSTOP,Add+i,0));
    ruleList=control(WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|WS_TABSTOP,0,0);ListView_SetExtendedListViewStyle(ruleList,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_GRIDLINES);
    const wchar_t* cols[]={L"方向",L"监听",L"目标",L"发行版",L"操作记录"};for(int i=0;i<5;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=(LPWSTR)cols[i];col.cx=150;ListView_InsertColumn(ruleList,i,&col);}
    usbInfo=control(L"STATIC",L"将 Windows 上的 USB 设备接入 WSL，例如 ESP32 开发板。",0,0,1);
    usbHint=control(L"STATIC",L"先刷新设备并填写 BUSID → 首次共享 → 接入 WSL。\r\n需要 usbipd-win；共享操作需管理员权限，接入后的设备供 WSL 2 使用。",0,0,1);
    busLabel=control(L"STATIC",L"设备 BUSID",0,0,1);busEdit=control(L"EDIT",L"1-2",ES_AUTOHSCROLL|WS_TABSTOP,0,1);
    const wchar_t* ul[]={L"刷新设备",L"首次共享",L"接入 WSL",L"断开",L"取消共享"};for(int i=0;i<5;++i)usbButtons.push_back(control(L"BUTTON",ul[i],WS_TABSTOP,UsbList+i,1));
    usbOutput=control(L"EDIT",L"点击“刷新设备”获取 USB 列表。",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_HSCROLL|ES_AUTOHSCROLL|WS_TABSTOP,0,1);
    logBox=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);SendMessageW(logBox,EM_SETLIMITTEXT,200000,0);
    fonts();layout();refreshRules();log(L"就绪。顶部选择发行版；操作记录不代表实时运行状态。");
}
void preview(HWND h,const S& filename=L"native-preview.bmp") {
    RECT rect;GetWindowRect(h,&rect);int width=rect.right-rect.left,height=rect.bottom-rect.top;
    HDC screen=GetDC(h),memory=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    HGDIOBJ old=SelectObject(memory,bitmap);SendMessageW(h,WM_PRINT,(WPARAM)memory,PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<unsigned char> pixels((size_t)width*height*4);SelectObject(memory,old);GetDIBits(screen,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS);
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);S path=exe;path=path.substr(0,path.find_last_of(L"\\")+1)+filename;
    Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr)};
    if(file.h!=INVALID_HANDLE_VALUE){BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+(DWORD)pixels.size();DWORD n;
        WriteFile(file.h,&header,sizeof(header),&n,nullptr);WriteFile(file.h,&info.bmiHeader,sizeof(BITMAPINFOHEADER),&n,nullptr);WriteFile(file.h,pixels.data(),(DWORD)pixels.size(),&n,nullptr);}
    DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(h,screen);
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE:window=h;dpi=GetDpiForWindow(h);create();if(smoke)SetTimer(h,2,1500,nullptr);else SetTimer(h,1,100,nullptr);return 0;
    case WM_TIMER:KillTimer(h,wp);if(wp==1)discover();else if(wp==2){
        UINT initial=dpi;
        for(UINT testDpi:{96u,144u,192u,initial}) {
            RECT rect{-10000,-10000,-10000+MulDiv(1060,testDpi,96),-10000+MulDiv(820,testDpi,96)};
            SendMessageW(h,WM_DPICHANGED,MAKELONG(testDpi,testDpi),(LPARAM)&rect);
            LOGFONTW lf{};GetObjectW(font,sizeof(lf),&lf);RECT input;GetWindowRect(listenEdit,&input);
            if(lf.lfHeight!=-MulDiv(10,testDpi,72)||input.bottom-input.top!=MulDiv(30,testDpi,96))smokeFailed=true;
        }
        preview(h);page=1;TabCtrl_SetCurSel(tabs,1);layout();preview(h,L"native-usb-preview.bmp");PostMessageW(h,WM_CLOSE,0,0);
    }return 0;
    case WM_SIZE:if(heading)layout();return 0;
    case WM_DPICHANGED:{dpi=HIWORD(wp);fonts();RECT* r=(RECT*)lp;SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0;}
    case WM_GETMINMAXINFO:{auto p=(MINMAXINFO*)lp;UINT d=GetDpiForWindow(h);if(!d)d=96;p->ptMinTrackSize={MulDiv(900,d,96),MulDiv(760,d,96)};return 0;}
    case WM_COMMAND:if(LOWORD(wp)==Distros&&HIWORD(wp)==CBN_SELCHANGE)versionStatus();else if(HIWORD(wp)==BN_CLICKED)action(LOWORD(wp));return 0;
    case WM_NOTIFY:if(((NMHDR*)lp)->hwndFrom==tabs&&((NMHDR*)lp)->code==TCN_SELCHANGE){page=TabCtrl_GetCurSel(tabs);layout();}return 0;
    case WM_RESULT:{std::unique_ptr<Result> r((Result*)lp);busy=false;enable();try{if(!r->error.empty())log(r->error);else if(r->apply)r->apply();}catch(const Failure& e){log(e.text+L" 请核对实际状态。");}versionStatus();return 0;}
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)wp;
        if((HWND)lp==heading||(HWND)lp==subtitle){
            // Child windows are excluded from the parent's background paint.
            // An explicit brush is needed on real repaints, not NULL_BRUSH.
            static HBRUSH headerBrush=CreateSolidBrush(RGB(23,38,60));
            SetBkMode(dc,OPAQUE);SetBkColor(dc,RGB(23,38,60));
            SetTextColor(dc,(HWND)lp==heading?RGB(245,248,255):RGB(190,207,231));
            return (LRESULT)headerBrush;
        }
        SetBkMode(dc,OPAQUE);SetTextColor(dc,RGB(39,56,79));SetBkColor(dc,RGB(247,249,252));
        static HBRUSH bg=CreateSolidBrush(RGB(247,249,252));return (LRESULT)bg;
    }
    case WM_ERASEBKGND:{RECT rc;GetClientRect(h,&rc);HBRUSH bg=CreateSolidBrush(RGB(247,249,252));FillRect((HDC)wp,&rc,bg);DeleteObject(bg);rc.bottom=px(84);bg=CreateSolidBrush(RGB(23,38,60));FillRect((HDC)wp,&rc,bg);DeleteObject(bg);return 1;}
    case WM_CLOSE:if(busy){log(L"正在执行操作，请等待完成后关闭（命令有超时限制）。");return 0;}DestroyWindow(h);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }return DefWindowProcW(h,msg,wp,lp);
}
int selftest() {
    require(quote(L"a b")==L"\"a b\"",L"argument quoting");require(quote(L"a\\")==L"\"a\\\\\"",L"trailing slash quoting");
    auto d=parseDistros(L"  NAME STATE VERSION\n* Ubuntu-24.04   Running   2\n  Debian Test   已停止   1\n");require(d.size()==2&&d[0].isDefault&&d[1].version==1,L"WSL parser");
    ipv4(L"127.0.0.1");busid(L"1-2.3");require(port(L"65535")==65535,L"port");bool rejected=false;try{port(L"80;id");}catch(Failure&){rejected=true;}require(rejected,L"port rejection");
    return 0;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR args,int show) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WSADATA data;WSAStartup(MAKEWORD(2,2),&data);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(S(args)==L"--self-test")return selftest();
        if(S(args)==L"--detect-test"){auto found=parseDistros(run(L"wsl.exe",{L"--list",L"--verbose"}));return found.empty()?2:0;}
        smoke=S(args)==L"--smoke-test";
        require(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)!=0,L"无法启用 Per-Monitor V2 DPI");
        PWSTR local=nullptr;require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)),L"无法定位用户数据目录");dataPath=S(local)+L"\\WSLTunnel";CoTaskMemFree(local);CreateDirectoryW(dataPath.c_str(),nullptr);iniPath=dataPath+L"\\native-rules.ini";
        S loadError;try{load();}catch(const Failure& e){loadError=e.text;rules.clear();}
        INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_STANDARD_CLASSES|ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&cc);
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance;wc.lpfnWndProc=proc;wc.lpszClassName=L"WSLTunnelNative";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);RegisterClassExW(&wc);
        UINT d=GetDpiForSystem();HWND h=CreateWindowExW(0,wc.lpszClassName,L"WSL Tunnel — Native",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,smoke?-10000:CW_USEDEFAULT,smoke?-10000:CW_USEDEFAULT,MulDiv(1060,d,96),MulDiv(820,d,96),nullptr,nullptr,instance,nullptr);
        require(h!=nullptr,L"无法创建窗口");ShowWindow(h,smoke?SW_SHOWNOACTIVATE:show);UpdateWindow(h);if(!loadError.empty())log(loadError);
        MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        if(font)DeleteObject(font);if(titleFont)DeleteObject(titleFont);CoUninitialize();WSACleanup();return smokeFailed?3:0;
    }catch(const Failure& e){if(S(args)==L"--self-test"||S(args)==L"--smoke-test"||S(args)==L"--detect-test"){
        wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);S path=exe;path=path.substr(0,path.find_last_of(L"\\")+1)+L"native-test-error.txt";
        Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr)};if(file.h!=INVALID_HANDLE_VALUE){DWORD n;S message=L"\ufeff"+e.text;WriteFile(file.h,message.data(),(DWORD)(message.size()*2),&n,nullptr);}return 1;
    }MessageBoxW(nullptr,e.text.c_str(),L"WSL Tunnel",MB_OK|MB_ICONERROR);return 1;}
}
