#include "model.hpp"
#include <iostream>
using namespace result_viewer;
int checks = 0;
void check(bool ok, const char* why) { require(ok, why); ++checks; }
template<class F> void rejects(F f, const char* why) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    check(caught, why);
}
Json setting() {
    return {{"map",{{"width",2},{"height",2},{"cells",{{0,3},{2,1}}}}},
            {"fuelLimits",10},{"spots",Json::array({{{"pos",0},{"brand",42},{"stocks",2}}})},
            {"daySteps",{2,2}}};
}
Json daily() {
    return {{"day",0},{"agents",Json::array({{{"pos",0},{"fuel",7},{"kind",0}},
                                             {{"pos",1},{"fuel",10},{"kind",1}}})},
            {"traffics",Json::array({{{"pos",1},{"status",2}}})}};
}
Json report() {
    const Json state = {{"agents",Json::array({{{"cell",0},{"fuel",6}},{{"cell",1},{"fuel",10}}})},
                        {"spot_stock",{1}}};
    return {{"format","c_staged.local_report.v1"},{"day",0},{"used_fallback",false},
            {"objective",{1,1,1}},{"solver_completed",true},
            {"next_history_draft",{{"map_config",setting()},{"agent_kinds",{0,1}}}},
            {"replay",{{"valid",true},{"committed_state",state},
             {"trace",Json::array({{{"step",1},{"state",state},{"refueled_agent_ids",Json::array()}},
                                   {{"step",2},{"state",state},{"refueled_agent_ids",{0}}}})}}}};
}
Json record(std::string label, std::string status, int revision) {
    return {{"report",report()},{"label",label},{"status",status},{"revision",revision}};
}
Json journal() {
    return {{"format","c_staged.match_journal.v1"},{"status","running"},{"setting",setting()},
            {"days",Json::array({{{"state",daily()},{"ambiguous_payload",nullptr},
             {"attempts",Json::array({record("fallback","accepted",1),record("solver","accepted",2)})}}})}};
}
void selftests(const std::filesystem::path& temp) {
    auto j = journal();
    const auto original = j;
    auto doc = parse(j);
    check(j==original,"parser must not mutate input");
    check(doc.days[0].preferred==1,"latest accepted default");
    check(doc.spots[0].brand==42,"original brand ID preserved");
    check(doc.days[0].traffic[1]==2,"traffic projected");
    const auto& a=doc.days[0].attempts[1];
    check(a.frames.size()==3 && a.frames[0].agents[0].fuel==7 && a.frames[1].agents[0].fuel==6,"recorded fuel, not simulation");
    check(a.frames[0].stock[0]==2 && a.frames[1].stock[0]==1,"recorded stock");
    check(a.frames[2].refueled==std::vector<int>{0},"refuel IDs projected");
    j["days"][0]["attempts"].push_back(record("solver","unknown",3));
    j["days"][0]["ambiguous_payload"]={{-2},{-2}};
    doc=parse(j);
    check(doc.days[0].preferred==1 && doc.days[0].unresolved,"unknown never supersedes acceptance");
    j["days"][0]["attempts"].push_back(record("solver","rejected",4));
    check(parse(j).days[0].preferred==1,"reject never supersedes acceptance");
    j["days"][0]["attempts"]=Json::array({record("solver","intent",-1)});
    check(parse(j).days[0].attempts[0].status=="intent","intent not acceptance");
    j["days"][0]["attempts"]=Json::array();
    check(parse(j).days[0].preferred==-1,"day without answer");
    auto r=report();
    auto standalone=parse(r,daily());
    check(standalone.days[0].attempts[0].status=="unrecorded","standalone acceptance unknown");
    r.erase("solver_completed");
    check(parse(r,daily()).days[0].attempts[0].label=="local report","validation report is not necessarily solver");
    r["used_fallback"]=true;
    check(parse(r,daily()).days[0].attempts[0].label=="fallback","fallback flag");
    rejects([&]{parse(r);},"missing day fails");
    r=report(); r["replay"]["valid"]=false;
    rejects([&]{parse(r,daily());},"invalid replay fails");
    r=report(); r["replay"]["trace"].erase(1);
    rejects([&]{parse(r,daily());},"truncated trace fails");
    r=report(); r["replay"]["trace"][0]["step"]=2;
    rejects([&]{parse(r,daily());},"unordered trace fails");
    r=report(); r["replay"]["trace"][0]["state"]["agents"][0]["cell"]=99;
    rejects([&]{parse(r,daily());},"bad cell fails");
    r=report(); r["replay"]["committed_state"]["agents"][0]["fuel"]=9;
    rejects([&]{parse(r,daily());},"inconsistent final state fails");
    auto d=daily(); d["day"]=1;
    rejects([&]{parse(report(),d);},"wrong day fails");
    d=daily(); d["agents"][0]["kind"]=1;
    rejects([&]{parse(report(),d);},"wrong kinds fails");
    j=journal(); j["days"][0]["attempts"][0]["report"]["next_history_draft"]["map_config"]["fuelLimits"]=9;
    rejects([&]{parse(j);},"wrong map fails");
    rejects([&]{parse(Json::array());},"answer array not viewer input");
    rejects([&]{auto bad=Json::parse("{bad"); (void)bad;},"malformed JSON fails");
    j=journal(); j["days"][0]["attempts"][0]["report"]["objective"][0]=1.5;
    rejects([&]{parse(j);},"fractional score fails");
    j=journal(); j["days"][0]["attempts"][0]["revision"]=-1;
    rejects([&]{parse(j);},"acceptance without receipt revision fails");
    auto emptyMap=setting(); emptyMap["spots"]=Json::array();
    j=journal(); j["setting"]=emptyMap; j["days"][0]["attempts"]=Json::array();
    check(parse(j).spots.empty(),"no Spot input");
    auto largeMap=emptyMap;
    largeMap["map"]={{"width",32},{"height",32},{"cells",std::vector<std::vector<int>>(32,std::vector<int>(32,0))}};
    largeMap["daySteps"]={256};
    j["setting"]=largeMap;
    auto largeDay=daily(); largeDay["agents"]=Json::array();
    for(int i=0;i<8;++i) largeDay["agents"].push_back({{"pos",1023-i},{"fuel",10},{"kind",i%2}});
    j["days"][0]["state"]=largeDay;
    auto largeDoc=parse(j);
    check(largeDoc.terrain.size()==1024 && largeDoc.days[0].kinds.size()==8 && largeDoc.days[0].steps==256,"maximum display dimensions");
    std::filesystem::create_directories(temp);
    const auto path=temp/"journal.json";
    {std::ofstream out(path);out<<journal();}
    Store store;
    check(store.open(path),"open succeeds");
    {std::ofstream out(path);out<<"{";}
    check(!store.reload() && store.document->days[0].attempts.size()==2 && !store.error.empty(),"failed reload preserves last good snapshot");
    {std::ofstream out(path);auto next=journal();next["days"][0]["next_state_verified"]=true;out<<next;}
    check(store.reload() && store.error.empty() && store.document->days[0].next_state_verified,"reload refreshes snapshot");
    {std::ofstream out(temp/"report.json");out<<report();}
    rejects([&]{load(temp/"report.json");},"missing sibling rejected");
    {std::ofstream out(temp/"day.json");out<<daily();}
    check(load(temp/"report.json").days.size()==1,"standalone file");
    {std::ofstream out(temp/"map.json");auto m=setting();m["fuelLimits"]=9;out<<m;}
    rejects([&]{load(temp/"report.json");},"wrong sibling map");
}
void real_record(const std::filesystem::path& path) {
    const auto root=read(path);
    const auto doc=parse(root);
    check(doc.width==root["setting"]["map"]["width"],"real map");
    for(size_t i=0;i<doc.days.size();++i) {
        const auto& day=doc.days[i];
        const auto& raw=root["days"][i];
        for(size_t k=0;k<day.attempts.size();++k) {
            const auto& a=day.attempts[k]; const auto& r=raw["attempts"][k]["report"];
            check(Json(a.objective)==r["objective"],"real objective");
            for(size_t t=1;t<a.frames.size();++t) {
                const auto& recorded=r["replay"]["trace"][t-1];
                for(size_t p=0;p<a.frames[t].agents.size();++p) {
                    check(a.frames[t].agents[p].cell==recorded["state"]["agents"][p]["cell"],"real position");
                    check(a.frames[t].agents[p].fuel==recorded["state"]["agents"][p]["fuel"],"real fuel");
                }
                check(Json(a.frames[t].stock)==recorded["state"]["spot_stock"],"real stock");
                check(Json(a.frames[t].refueled)==recorded["refueled_agent_ids"],"real refuel");
            }
        }
    }
    std::cout<<"Real journal: "<<doc.days.size()<<" days; all recorded states match\n";
}
int main(int argc,char** argv) {
    try {
        require(argc>=2,"test_model <empty temporary directory> [journal.json]");
        selftests(std::filesystem::u8path(argv[1]));
        for(int i=2;i<argc;++i) real_record(std::filesystem::u8path(argv[i]));
        std::cout<<checks<<" checks PASS\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
