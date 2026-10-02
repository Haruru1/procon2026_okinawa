#pragma once
#include <Siv3D.hpp>
#include "model.hpp"

namespace result_viewer_ui {
using namespace s3d;
const ColorF ink{0.10,0.13,0.16}, muted{0.32,0.37,0.41}, paper{0.94,0.96,0.97};
const ColorF accent{0.0,0.40,0.33}, alarm{0.73,0.17,0.14};
const ColorF grid{0.48,0.54,0.57}, selection{0.96,0.69,0.10};
const std::array<Color,8> vehicleColors{{{0,87,148},{113,65,150},{0,109,75},{160,92,6},
    {0,104,125},{111,78,45},{71,78,142},{57,70,80}}};
inline String str(const std::string& text) { return Unicode::FromUTF8(text); }
inline void fit(const Font& font, String text, Vec2 pos, double width, ColorF color=ink) {
    if(width<=0) return;
    while(!text.empty() && font(text).region().w>width) {
        text.pop_back();
        if(font(text+U"…").region().w<=width) { text+=U"…"; break; }
    }
    font(text).draw(pos,color);
}
inline String statusText(const std::string& status) {
    if(status=="accepted") return U"受理済み";
    if(status=="rejected") return U"回答拒否";
    if(status=="unknown") return U"受理不明";
    if(status=="intent") return U"送信予定 / 結果未記録";
    if(status=="not_sent") return U"未送信";
    return U"送信・受理記録なし";
}
struct Toolbar {
    Font font{14};
    String hint;
    HashTable<char32,Texture> icons;
    bool button(RectF rect, char32 code, StringView label, bool enabled=true, bool active=false, bool input=true) {
        const bool hover=input && enabled && rect.mouseOver();
        if(hover || active) rect.draw(ColorF{0.88,0.92,0.91});
        if(!icons.contains(code)) icons.emplace(code,Texture{Icon{uint32(code)},20});
        icons.at(code).drawAt(rect.center(),enabled?ink:ColorF{0.72});
        if(hover) { Cursor::RequestStyle(CursorStyle::Hand); hint=label; }
        return input && enabled && rect.leftClicked();
    }
    void tooltip() {
        if(hint.empty()) return;
        const double w=Min(font(hint).region().w+18,Scene::Width()-16.0);
        const Vec2 p{Max(8.0,Min(Cursor::PosF().x+12,Scene::Width()-w-8)),
                     Min(Cursor::PosF().y+24,Scene::Height()-30.0)};
        RectF{p,w,27}.draw(ColorF{0.15});
        fit(font,hint,p+Vec2{9,3},w-18,Palette::White);
    }
};
struct PanelScroll {
    double offset=0,grabY=0;
    bool dragging=false;
    static double limit(double contentHeight,double viewHeight) {
        return Max(0.0,contentHeight-viewHeight);
    }
    RectF thumb(const RectF& track,double contentHeight,double viewHeight) const {
        const RectF rail=track.stretched(-3);
        const double length=Min(rail.h,Max(36.0,rail.h*Min(1.0,viewHeight/Max(1.0,contentHeight))));
        const double range=limit(contentHeight,viewHeight);
        return {rail.x,rail.y+(range>0?(rail.h-length)*offset/range:0),rail.w,length};
    }
    static double fromThumb(double y,const RectF& track,double length,double range) {
        return track.h>length?Clamp((y-track.y)/(track.h-length),0.0,1.0)*range:0;
    }
    void update(const RectF& view,const RectF& track,double contentHeight) {
        const double range=limit(contentHeight,view.h);
        offset=Clamp(offset,0.0,range);
        if(!MouseL.pressed()) dragging=false;
        if(view.mouseOver() && !dragging) offset=Clamp(offset+Mouse::Wheel()*42.0,0.0,range);
        auto bar=thumb(track,contentHeight,view.h);
        if(range>0 && track.leftClicked()) {
            if(bar.mouseOver()) { dragging=true;grabY=Cursor::PosF().y-bar.y; }
            else offset=Clamp(offset+(Cursor::PosF().y<bar.y?-1:1)*view.h*0.85,0.0,range);
        }
        if(dragging) offset=fromThumb(Cursor::PosF().y-grabY,track.stretched(-3),bar.h,range);
        if(range<=0) dragging=false;
    }
    void draw(const RectF& track,double contentHeight,double viewHeight,Toolbar& toolbar) const {
        // The scrollbar has its own gutter and must not inherit the content scissor.
        const ScopedRenderStates2D unclipped{RasterizerState::SolidCullNone};
        track.stretched(3,0).draw(ColorF{0.94,0.95,0.96});
        track.draw(ColorF{0.82,0.85,0.88}).drawFrame(2,ColorF{0.34,0.40,0.46});
        const bool overflow=limit(contentHeight,viewHeight)>0;
        const auto bar=thumb(track,contentHeight,viewHeight);
        bar.draw(overflow && (dragging || bar.mouseOver())?ColorF{0.23,0.30,0.37}:ColorF{0.38,0.45,0.51})
            .drawFrame(1,ColorF{0.23,0.29,0.35});
        for(int dy=-4;dy<=4;dy+=4)
            Line{bar.x+3,bar.center().y+dy,bar.rightX()-3,bar.center().y+dy}.draw(1.5,Palette::White);
        if(track.mouseOver()) {
            toolbar.hint=overflow?U"情報パネルをスクロール":U"情報パネル: 全項目を表示中";
            if(overflow) Cursor::RequestStyle(CursorStyle::Hand);
        }
    }
};
inline Vec2 hexPosition(int cell,int width) {
    const int row=cell/width,col=cell%width;
    return {Sqrt(3.0)*(col+((row&1)?0.0:0.5)),row*1.5};
}
inline Polygon hexagon(Vec2 p,double size) {
    Array<Vec2> points;
    for(int i=0;i<6;++i) {
        const double angle=Math::Pi/180.0*(60*i-30);
        points << p+Vec2{Cos(angle),Sin(angle)}*size;
    }
    return Polygon{points};
}
inline ColorF ground(int terrain,int traffic) {
    if(terrain==1) return ColorF{0.62,0.66,0.63};
    if(terrain==2) return ColorF{0.62,0.79,0.86};
    if(terrain==3) {
        if(traffic==2) return ColorF{0.77,0.54,0.20};
        if(traffic==1) return ColorF{0.96,0.79,0.39};
        return ColorF{0.81,0.84,0.88};
    }
    return ColorF{0.92,0.95,0.89};
}
inline void run() {
    Window::SetTitle(U"ヘキサうどん | 結果ビューア");
    Window::SetStyle(WindowStyle::Sizable);
    Window::Resize(1440,900);
    Window::SetMinimumFrameBufferSize({900,560});
    Scene::SetResizeMode(ResizeMode::Actual);
    Scene::SetBackground(paper);
    const Font title{20,Typeface::Bold},normal{16},small{13},score{24,Typeface::Bold};
    Toolbar toolbar;
    PanelScroll panelScroll;
    result_viewer::Store store;
    int dayIndex=0,attemptIndex=-1,selectedAgent=-1,selectedSpot=-1;
    double frameIndex=0,zoom=1;
    Vec2 pan{0,0};
    auto chooseDay=[&](int index) {
        dayIndex=index;
        const auto& day=store.document->days.at(size_t(index));
        attemptIndex=day.preferred;
        frameIndex=attemptIndex<0?0:day.steps;
        selectedSpot=selectedAgent=-1;
        panelScroll={};
    };
    auto open=[&](const FilePath& path,bool reload) {
        const int oldDay=store.document?store.document->days[dayIndex].index:-1;
        const double oldScroll=panelScroll.offset;
        if(store.open(std::filesystem::path{path.toWstr()})) {
            int index=int(store.document->days.size())-1;
            if(reload) for(size_t i=0;i<store.document->days.size();++i)
                if(store.document->days[i].index==oldDay) index=int(i);
            chooseDay(index);
            if(reload && store.document->days[index].index==oldDay) panelScroll.offset=oldScroll;
            zoom=1; pan={0,0};
        }
    };
    // QA mode reads a static file and captures the same rendering path, then exits.
    const auto& args=System::GetCommandLineArgs();
    // Packaging exports the engine's own bundled third-party notices, not guessed licenses.
    if(args.size()==3 && args[1]==U"--export-licenses") {
        TextWriter out{args[2],TextEncoding::UTF8_NO_BOM};
        if(!out) throw Error{U"Cannot write license notices"};
        for(const auto& info:LicenseManager::EnumLicenses()) {
            out.writeln(info.title);
            out.writeln(info.copyright);
            out.writeln(info.text);
            out.writeln(U"\n----------------------------------------\n");
        }
        return;
    }
    String capture;
    int qaDay=-1,qaStep=-1,qaAgent=-1,qaWidth=1440,qaHeight=900;
    bool qaPanelBottom=false;
    for(size_t i=1;i<args.size();++i) {
        if(args[i]==U"--capture" && i+1<args.size()) capture=args[++i];
        else if(args[i]==U"--day-index" && i+1<args.size()) qaDay=ParseOr<int>(args[++i],-1);
        else if(args[i]==U"--step" && i+1<args.size()) qaStep=ParseOr<int>(args[++i],-1);
        else if(args[i]==U"--width" && i+1<args.size()) qaWidth=Clamp(ParseOr<int>(args[++i],1440),900,2560);
        else if(args[i]==U"--height" && i+1<args.size()) qaHeight=Clamp(ParseOr<int>(args[++i],900),560,1440);
        else if(args[i]==U"--panel-bottom") qaPanelBottom=true;
        else if(args[i]==U"--agent-index" && i+1<args.size()) qaAgent=ParseOr<int>(args[++i],-1);
        else open(args[i],false);
    }
    if(!capture.empty()) {
        Window::ResizeActual(qaWidth,qaHeight);
        ScreenCapture::SetScreenshotDirectory(FileSystem::ParentPath(capture));
        capture=FileSystem::FileName(capture);
    }
    if(store.document && qaDay>=0 && qaDay<int(store.document->days.size())) chooseDay(qaDay);
    if(store.document && qaStep>=0) frameIndex=Min(qaStep,store.document->days[dayIndex].steps);
    if(!capture.empty() && store.document && qaAgent>=0 && qaAgent<int(store.document->days[dayIndex].kinds.size())) selectedAgent=qaAgent;
    int frames=0;
    while(System::Update()) {
        toolbar.hint.clear();
        const double w=Scene::Width(),h=Scene::Height(),side=304,sx=w-side;
        RectF{0,0,w,56}.draw(Palette::White);
        Line{0,56,w,56}.draw(2,ColorF{0.72,0.76,0.79});
        title(U"ヘキサうどん").draw(18,15,ink);
        small(U"RESULT VIEWER").draw(170,23,muted);
        if(toolbar.button({300,9,38,38},U'\uf07c',U"結果JSONを開く"))
            if(const auto file=Dialog::OpenFile({FileFilter::JSON()})) open(*file,false);
        if(toolbar.button({342,9,38,38},U'\uf2f1',U"再読み込み",!store.path.empty()) ||
            (KeyF5.down() && !store.path.empty())) open(Unicode::FromWstring(store.path.wstring()),true);
        small(U"読取専用 / 保存済み記録").draw(w-250,23,muted);
        if(toolbar.button({w-47,8,38,40},U'\uf00d',U"ビューアを閉じる")) {
            System::Exit();
            break;
        }
        if(!store.document) {
            title(U"結果JSON未読込").drawAt(w/2,h/2-25,muted);
            if(!store.error.empty()) fit(small,str(store.error),{24,h-64},w-48,alarm);
        } else {
            const auto& doc=*store.document;
            if(toolbar.button({406,9,32,38},U'\uf053',U"前の日",dayIndex>0)) chooseDay(dayIndex-1);
            if(toolbar.button({556,9,32,38},U'\uf054',U"次の日",dayIndex+1<int(doc.days.size()))) chooseDay(dayIndex+1);
            title(U"{}日目"_fmt(doc.days[dayIndex].index+1)).drawAt(497,29,ink);
            const auto& d=doc.days[dayIndex];
            const RectF panel{sx,56,side,h-80},contentView{sx,56,side-28,h-80};
            const RectF track{w-24,64,20,h-96};
            const double spotY=Max(455.0,363+d.kinds.size()*27.0),contentHeight=spotY+100-panel.y;
            if(!capture.empty() && qaPanelBottom && frames==0) panelScroll.offset=PanelScroll::limit(contentHeight,panel.h);
            panelScroll.update(panel,track,contentHeight);
            const bool panelInput=contentView.mouseOver() && !panelScroll.dragging;
            // Change selection before taking references, so labels and route use one attempt.
            if(panelInput && attemptIndex>0 && RectF{sx+204,124-panelScroll.offset,28,30}.leftClicked()) {--attemptIndex;frameIndex=d.steps;}
            if(panelInput && attemptIndex+1<int(d.attempts.size()) && RectF{sx+237,124-panelScroll.offset,28,30}.leftClicked()) {++attemptIndex;frameIndex=d.steps;}
            const auto* a=attemptIndex<0?nullptr:&d.attempts[size_t(attemptIndex)];
            const auto& f=a?a->frames[size_t(Clamp(int(frameIndex),0,d.steps))]:d.initial;
            const RectF area{12,70,sx-24,h-182};
            const double base=Min(area.w/(Sqrt(3.0)*(doc.width+1)),area.h/(1.5*doc.height+0.5));
            if(area.mouseOver()) {
                zoom=Clamp(zoom*(1-Mouse::Wheel()*0.1),1.0,4.0);
                if(MouseR.pressed()) pan+=Cursor::DeltaF();
            }
            if(RectF{sx-50,66,36,36}.leftClicked()) {zoom=1;pan={0,0};}
            const Vec2 midpoint{Sqrt(3.0)*(doc.width-0.5)*0.5,1.5*(doc.height-1)*0.5};
            auto pos=[&](int cell){return area.center()+(hexPosition(cell,doc.width)-midpoint)*base*zoom+pan;};
            {
                const ScopedRenderStates2D clip{RasterizerState::SolidCullNoneScissor};
                Graphics2D::SetScissorRect(Rect{int(area.x),int(area.y),int(area.w),int(area.h)});
                for(int cell=0;cell<int(doc.terrain.size());++cell)
                    hexagon(pos(cell),base*zoom).draw(ground(doc.terrain[cell],d.traffic[cell])).drawFrame(2.0,grid);
                if(a) for(int id=0;id<int(d.kinds.size());++id) {
                    const bool strong=selectedAgent<0 || selectedAgent==id;
                    const ColorF color{vehicleColors[id],strong?1.0:0.22};
                    int prev=a->frames[0].agents[id].cell;
                    for(size_t t=1;t<a->frames.size();++t) {
                        int cell=a->frames[t].agents[id].cell;
                        if(prev!=cell) {
                            const Line segment{pos(prev),pos(cell)};
                            const double width=selectedAgent==id?4.5:3.0;
                            segment.draw(width+2.0,ColorF{1.0,strong?0.92:0.24});
                            segment.draw(width,color);
                        }
                        prev=cell;
                    }
                    Circle{pos(a->frames[0].agents[id].cell),4.5}.drawFrame(2.0,color);
                }
                for(size_t i=0;i<doc.spots.size();++i) {
                    const Vec2 p=pos(doc.spots[i].cell);
                    const double size=Clamp(base*zoom*0.54,5.0,12.0);
                    RectF{Arg::center=p,size*2,size*2}.rotated(Math::Pi/4).draw(f.stock[i]?ColorF{1.0}:ColorF{0.76,0.80,0.83}).drawFrame(2.0,ink);
                    if(size>=7) small(i).drawAt(p,ink);
                    if(area.mouseOver() && RectF{Arg::center=p,24,24}.mouseOver()) {
                        toolbar.hint=U"Spot {} / 系列 {} / 在庫 {}"_fmt(i,doc.spots[i].brand,f.stock[i]);
                        if(MouseL.down()) selectedSpot=int(i);
                    }
                    if(selectedSpot==int(i)) {
                        const auto mark=RectF{Arg::center=p,size*2+8,size*2+8}.rotated(Math::Pi/4);
                        mark.drawFrame(6,Palette::White).drawFrame(3,selection);
                    }
                }
                for(int id=0;id<int(f.agents.size());++id) {
                    Vec2 p=pos(f.agents[id].cell);
                    int order=0,count=0;
                    for(int j=0;j<int(f.agents.size());++j) if(f.agents[j].cell==f.agents[id].cell) {
                        if(j<id) ++order;
                        ++count;
                    }
                    if(count>1) {
                        const double angle=Math::TwoPi*order/count;
                        Vec2 shift{Cos(angle)*13,Sin(angle)*13};
                        Line{p,p+shift}.draw(2,ink);p+=shift;
                    }
                    const ColorF color{vehicleColors[id]};
                    if(d.kinds[id]) RectF{Arg::center=p,22,22}.draw(color).drawFrame(2.0,Palette::White);
                    else Circle{p,11}.draw(color).drawFrame(2.0,Palette::White);
                    if(selectedAgent==id) Circle{p,18}.drawFrame(6,Palette::White).drawFrame(3,selection);
                    small(id).drawAt(p,Palette::White);
                    if(std::find(f.refueled.begin(),f.refueled.end(),id)!=f.refueled.end()) Circle{p,14}.drawFrame(3,accent);
                }
            }
            toolbar.button({sx-50,66,36,36},U'\uf065',U"マップ全体");
            panel.draw(Palette::White);
            Line{sx,56,sx,h-24}.draw(2,ColorF{0.72,0.76,0.79});
            {
            const ScopedRenderStates2D clip{RasterizerState::SolidCullNoneScissor};
            Graphics2D::SetScissorRect(Rect{int(contentView.x),int(contentView.y),int(contentView.w),int(contentView.h)});
            // Move drawing and cursor together; panelInput blocks clipped/off-panel controls.
            const Transformer2D scrollTransform{Mat3x2::Translate(0,-panelScroll.offset),TransformCursor::Yes};
            const double x=sx+16;
            fit(small,U"予測: 新規系列 / 当日系列 / 玉数",{x,72},side-40,muted);
            fit(score,a?U"({}, {}, {})"_fmt(a->objective[0],a->objective[1],a->objective[2]):U"未計算",{x,94},side-40);
            fit(small,a?str(a->label):U"回答記録なし",{x,132},178);
            // Render navigation over the sidebar, matching the hit regions above.
            toolbar.button({sx+204,124,28,30},U'\uf053',U"前の回答",attemptIndex>0,false,panelInput);
            toolbar.button({sx+237,124,28,30},U'\uf054',U"次の回答",attemptIndex+1<int(d.attempts.size()),false,panelInput);
            fit(normal,a?statusText(a->status):U"未送信",{x,161},side-40,
                a && a->status=="accepted"?accent:a && (a->status=="rejected" || a->status=="unknown")?alarm:muted);
            fit(small,a?U"回答 {} / {}   revision {}"_fmt(attemptIndex+1,d.attempts.size(),a->revision):U"回答 0件",{x,187},side-40,muted);
            fit(small,d.unresolved?U"警告: 受理不明の送信あり":(!d.solver_error.empty()?U"警告: solverエラー記録あり":
                d.next_state_verified?U"翌日状態: 照合済み":U"翌日状態: 未照合"),{x,210},side-40,
                d.unresolved || !d.solver_error.empty()?alarm:muted);
            fit(small,a && a->seconds?U"評価処理（記録） {:.3f} s"_fmt(*a->seconds):U"処理時間: 記録なし",{x,237},side-40,muted);
            fit(small,a && a->margin?U"応答時の締切余裕 {:.2f} s"_fmt(*a->margin):U"締切余裕: 記録なし",{x,258},side-40,muted);
            Line{x,289,w-24,289}.draw(2,ColorF{0.78,0.82,0.84});
            normal(U"車両").draw(x,300,ink);
            if(toolbar.button({w-57,295,32,32},U'\uf06e',U"全ルート表示",true,selectedAgent<0,panelInput)) selectedAgent=-1;
            small(U"ID / 種類       セル       燃料").draw(x,334,muted);
            for(int id=0;id<int(d.kinds.size());++id) {
                const double y=357+id*27;
                RectF row{x-6,y-2,side-38,26};
                if((panelInput && row.mouseOver()) || selectedAgent==id) row.draw(ColorF{0.98,0.94,0.79});
                if(selectedAgent==id) RectF{row.x,row.y,4,row.h}.draw(selection);
                if(panelInput && row.leftClicked()) selectedAgent=selectedAgent==id?-1:id;
                if(d.kinds[id]) RectF{Arg::center=Vec2{x+4,y+10},10,10}.draw(vehicleColors[id]);
                else Circle{x+4,y+10,5}.draw(vehicleColors[id]);
                small(U"{} {}"_fmt(id,d.kinds[id]?U"補給":U"巡回")).draw(x+15,y,ink);
                small(f.agents[id].cell).draw(x+111,y,ink);
                fit(small,d.kinds[id]?U"—":U"{} / {}"_fmt(f.agents[id].fuel,doc.fuel_limit),{x+174,y},side-214);
            }
            Line{x,spotY,w-24,spotY}.draw(2,ColorF{0.78,0.82,0.84});
            if(selectedSpot>=0) {
                const auto& s=doc.spots[selectedSpot];
                fit(normal,U"Spot {} / 系列 {}"_fmt(selectedSpot,s.brand),{x,spotY+12},side-40);
                fit(small,U"セル {}   在庫 {} / {}"_fmt(s.cell,f.stock[selectedSpot],s.stock),{x,spotY+41},side-40,muted);
            } else fit(small,U"Spot {} / 未選択"_fmt(doc.spots.size()),{x,spotY+15},side-40,muted);
            small(a?U"replay: 正常（記録値）":U"replay: 未記録").draw(x,spotY+66,muted);
            }
            fit(small,U"{} × {} / {} steps"_fmt(doc.width,doc.height,d.steps),{16,h-108},sx-280,muted);
            fit(small,U"◇ Spot   ○ 巡回   ■ 補給   緑輪: 補給   金枠: 選択",{16,h-87},sx-32,muted);
            small(U"道路: 灰 通常 / 黄 混雑 / 橙 渋滞").draw(sx-249,h-108,muted);
            small(int(frameIndex)==0?U"日初 / サーバー記録":U"計画replay / step {}"_fmt(int(frameIndex))).draw(16,h-57,ink);
            SimpleGUI::Slider(frameIndex,0.0,double(d.steps),{234,h-64},sx-270,a!=nullptr);
            frameIndex=Round(frameIndex);
            panelScroll.draw(track,contentHeight,panel.h,toolbar);
        }
        RectF{0,h-23,w,23}.draw(store.error.empty()?ColorF{0.90,0.92,0.92}:ColorF{1.0,0.86,0.83});
        const String footer=store.error.empty()?(store.path.empty()?U"ファイル未選択":Unicode::FromWstring(store.path.wstring())):
            (store.document?U"読込失敗（表示は前回の記録）: ":U"読込失敗: ")+str(store.error);
        fit(small,footer,{10,h-22},w-20,store.error.empty()?muted:alarm);
        if(RectF{0,h-23,w,23}.mouseOver()) toolbar.hint=footer;
        if(capture.empty()) toolbar.tooltip();
        if(!capture.empty()) {
            if(frames==8) ScreenCapture::SaveCurrentFrame(capture);
            if(frames>14) break;
        }
        ++frames;
    }
}
} // namespace result_viewer_ui
