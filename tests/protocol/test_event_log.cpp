#include "chipcae/io/event_log.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace chipcae::io;
namespace {
void expect(bool ok, const char* why) { if(!ok) throw std::runtime_error(why); }
Event make_event(std::uint64_t i) {
    Event e; e.seq=i; e.wall_time_unix_ns=1000000+static_cast<std::int64_t>(i);
    switch(i%6) {
    case 0: e.payload=KernelHello{"run-"+std::to_string(i),static_cast<std::uint32_t>(i%4),"v1"}; break;
    case 1: e.payload=Progress{i,static_cast<double>(i)*.25,.25,static_cast<double>(i%100)/100.0,"step"}; break;
    case 2: e.payload=Residual{"solver","temperature",i,static_cast<std::uint32_t>(i%20),1.25,3,"INTERNAL",1}; break;
    case 3: e.payload=LogLine{2,static_cast<std::int32_t>(i%8),"message", "INTERNAL",1}; break;
    case 4: e.payload=State{State::Value::running,i,static_cast<double>(i)}; break;
    default: e.payload=OutputAvailable{i,static_cast<double>(i),"out/"+std::to_string(i)+".vtkhdf","sha256"}; break;
    }
    return e;
}
void same(const Event& a,const Event& b) {
    expect(a.seq==b.seq && a.wall_time_unix_ns==b.wall_time_unix_ns,"record metadata mismatch");
    expect(a.payload.index()==b.payload.index(),"payload kind mismatch");
    if(auto x=std::get_if<KernelHello>(&a.payload)){auto y=std::get<KernelHello>(b.payload); expect(x->run_id==y.run_id&&x->attempt==y.attempt&&x->kernel_version==y.kernel_version,"hello mismatch");}
    if(auto x=std::get_if<Progress>(&a.payload)){auto y=std::get<Progress>(b.payload); expect(x->window==y.window&&x->time==y.time&&x->dt==y.dt&&x->fraction==y.fraction&&x->message==y.message,"progress mismatch");}
    if(auto x=std::get_if<Residual>(&a.payload)){auto y=std::get<Residual>(b.payload); expect(x->solver==y.solver&&x->field==y.field&&x->window==y.window&&x->iteration==y.iteration&&x->norm==y.norm&&x->flags==y.flags&&x->code==y.code&&x->error_code==y.error_code,"residual mismatch");}
    if(auto x=std::get_if<LogLine>(&a.payload)){auto y=std::get<LogLine>(b.payload); expect(x->level==y.level&&x->rank==y.rank&&x->message==y.message&&x->code==y.code&&x->error_code==y.error_code,"logline mismatch");}
    if(auto x=std::get_if<State>(&a.payload)){auto y=std::get<State>(b.payload); expect(x->value==y.value&&x->window==y.window&&x->time==y.time,"state mismatch");}
    if(auto x=std::get_if<OutputAvailable>(&a.payload)){auto y=std::get<OutputAvailable>(b.payload); expect(x->window==y.window&&x->time==y.time&&x->path==y.path&&x->checksum==y.checksum,"output mismatch");}
}
}
int main() {
    try {
        const auto path=std::filesystem::temp_directory_path()/"chipcae-event-log-test.bin";
        std::filesystem::remove(path);
        { EventLogWriter writer(path); for(std::uint64_t i=1;i<=1000;++i) writer.append(make_event(i)); writer.close(); }
        EventLogReader reader(path); const auto all=reader.read_from(1);
        expect(all.size()==1000,"expected 1000 records");
        for(std::size_t i=0;i<all.size();++i) same(make_event(i+1),all[i]);
        for(const auto seq : {std::uint64_t{1},std::uint64_t{500},std::uint64_t{1000},std::uint64_t{1001}}) {
            const auto suffix=reader.read_from(seq); expect(suffix.size()==(seq<=1000?1001-seq:0),"resume suffix length");
            if(!suffix.empty()) expect(suffix.front().seq==seq,"resume is inclusive");
        }
        // Reopen and append verifies durable records and append semantics.
        { EventLogWriter writer(path); auto e=make_event(1001); writer.append(e); }
        expect(reader.read_from(1001).size()==1,"reopen append");
        std::filesystem::remove(path);
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    return 0;
}
