#include "tpo_draw.h"
#include "../../ui/theme.h"
#include <chrono>
#include <cstdio>
#include <string_view>

void drawTpoProfiles(Ui& u, const ChartPane& pane, const TpoProfiles& model,
                     const CandleSeries& candles, float startBar, float barWidth,
                     bool split, int bracketMinutes, int marks, int64_t& selected, int extensions, int labelMask) {
  const auto& t = theme();
  const Rect area = pane.area;
  if (model.coarseSource) {
    u.draw.textFit({area.x+16,area.y+area.h*.45f,area.w-32,24},
        "Loading 30-minute TPO data", t.textDim, DrawList::Center);
    return;
  }
  const float dayWidth = (float)(1440.0 / candles.tf.value) * barWidth;
  const float cellWidth = dayWidth / (1440 / bracketMinutes + 12.0f);
  const Color valueInk = hexColor(0x2385eb);
  const Color outsideInk = hexColor(0x696e75);
  const Color poorInk=hexColor(0xf3cf55);
  struct LevelLine { Rect rect; Color ink; };
  static thread_local std::vector<LevelLine> levelLines;
  levelLines.clear();
  // Draw zones underneath every profile. Their left edge is the actual source
  // block, including in split mode; no duplicate column to the left.
  for(const auto& z:model.zones) {
    const bool poor=z.kind==TpoZoneKind::PoorHigh || z.kind==TpoZoneKind::PoorLow;
    const bool levelZone=poor || z.kind==TpoZoneKind::Poc || z.kind==TpoZoneKind::Vah || z.kind==TpoZoneKind::Val;
    const int ext=z.kind==TpoZoneKind::Poc?1:z.kind==TpoZoneKind::Vah?2:z.kind==TpoZoneKind::Val?4:z.kind==TpoZoneKind::Single?8:z.kind==TpoZoneKind::Tail?16:z.kind==TpoZoneKind::PoorHigh?32:64;
    const int visibility=poor?8:z.kind==TpoZoneKind::Single?2:z.kind==TpoZoneKind::Tail?4:z.kind==TpoZoneKind::Poc?1:0;
    if(visibility && !(marks&visibility)) continue;
    if(levelZone && !(extensions&ext)) continue;
    const auto& s=model.sessions[z.session];
    const auto& row=s.rows[z.first];
    const float baseX=area.x+(float)(tpoSessionBar(candles,s.day)-startBar)*barWidth;
    const float sourceX=baseX+(split?z.period:0)*cellWidth;
    const double right=tpoRetestRight(model,candles,z,split,startBar,barWidth);
    const float endX=!(extensions&ext)?sourceX+cellWidth:std::isfinite(right)?area.x+(float)right:area.x+area.w;
    const float top=pane.yOf((s.rows[z.last].tick+1)*model.step);
    const float bottom=pane.yOf(row.tick*model.step);
    if(bottom<area.y || top>area.y+area.h-16) continue;
    if(levelZone) {
      int last=s.lastPeriod;
      while(last>0 && !row.periods.test(last)) --last;
      const float from=baseX+(split?last+1:row.count())*cellWidth;
      const float y=z.kind==TpoZoneKind::PoorHigh || z.kind==TpoZoneKind::Vah?top:z.kind==TpoZoneKind::Poc?(top+bottom)*.5f:bottom;
      const Color lineInk=poor?poorInk:z.kind==TpoZoneKind::Poc?hexColor(0xffffff):t.textDim;
      const float left=std::max(from,area.x), right=std::min(endX,area.x+area.w);
      if(right>left) levelLines.push_back({{left,y,right-left,1},withAlpha(lineInk,.85f)});
    } else {
      const Color ink=z.kind==TpoZoneKind::Single?hexColor(0xd9ac59):hexColor(0x55b6a5);
      auto box=[&](float left,float right,float alpha) {
        left=std::max(left,area.x); right=std::min(right,area.x+area.w);
        const float y=std::max(top,area.y), endY=std::min(bottom,area.y+area.h-16);
        if(right<=left || endY<=y) return;
        u.draw.rectTile({left,y,right-left,endY-y},withAlpha(ink,alpha));
        if (top >= area.y) levelLines.push_back({{left,top,right-left,1},withAlpha(ink,.6f)});
        if (bottom <= area.y+area.h-16) levelLines.push_back({{left,bottom-1,right-left,1},withAlpha(ink,.6f)});
      };
      box(sourceX,sourceX+cellWidth,.32f);
      box(sourceX+cellWidth,endX,.12f);
    }
  }

  struct Label {float x,y; const char* text; Color ink;};
  static thread_local std::vector<Label> allLabels;
  allLabels.clear();
  for (const auto& s : model.sessions) {
    if (s.rows.empty()) continue;
    const float x = area.x + (float)(tpoSessionBar(candles,s.day)-startBar)*barWidth;
    if (x + dayWidth*s.days < area.x || x > area.x + area.w) continue;
    const auto& structure=model.structures[&s-model.sessions.data()];
    const float spanWidth=dayWidth*s.days;
    const int peak = s.rows[s.poc].count();
    const float profileWidth = (split ? s.lastPeriod+1 : peak)*cellWidth;
    for (size_t ri=0; ri<s.rows.size(); ++ri) {
      const auto& row = s.rows[ri];
      const float top = pane.yOf((row.tick+1)*model.step);
      const float bottom = pane.yOf(row.tick*model.step);
      if (bottom < area.y || top > area.y+area.h-16) continue;
      const float h = std::max(1.0f,bottom-top-1.0f);
      Color ink = (int)ri>=s.vaLow && (int)ri<=s.vaHigh ? valueInk : outsideInk;
      if ((marks&2) && structure.singles[ri]) ink=hexColor(0xd9ac59);
      if ((marks&4) && ((int)ri<structure.lowTail || (int)ri>=(int)s.rows.size()-structure.highTail))
        ink=hexColor(0x55b6a5);
      if((marks&8) && (((int)ri==0 && structure.poorLow) ||
          (ri+1==s.rows.size() && structure.poorHigh))) ink=poorInk;
      if((marks&1) && (int)ri==s.poc) ink=hexColor(0xffffff);
      if (!split && cellWidth < 1.8f) {
        u.draw.rectTile({x,top,row.count()*cellWidth,h},ink);
      } else {
        const int columns=split?s.lastPeriod+1:row.count();
        for(int p=0;p<columns;++p) if(!split || row.periods.test(p)) {
          const float cx=x+p*cellWidth;
          u.draw.rectTile({cx,top,std::max(.7f,cellWidth-1),h},ink);
        }
      }
      Rect hit{x,top,std::max(4.0f,profileWidth),std::max(3.0f,bottom-top)};
      if(u.hovered(hit)) {
        static char detail[240];
        int period=std::clamp((int)((u.input.mouseX-x)/cellWidth),0,s.lastPeriod);
        int clockMinute=(period*bracketMinutes)%1440;
        char when[80]="";
        if(split) snprintf(when,sizeof(when)," | day %d %02d:%02d UTC",period*bracketMinutes/1440+1,clockMinute/60,clockMinute%60);
        using namespace std::chrono;
        const year_month_day date{sys_days{days{s.day}}};
        snprintf(detail,sizeof(detail),"%04d-%02u-%02u UTC | %.8g - %.8g | %d TPOs%s%s%s%s",
            (int)date.year(),(unsigned)date.month(),(unsigned)date.day(),
            row.tick*model.step,(row.tick+1)*model.step,row.count(),
            (int)ri==s.poc?" | POC":"",s.partial?" | partial session":"",when,structure.singles[ri]?" | single print":((int)ri<structure.lowTail?" | buying tail":(int)ri>=(int)s.rows.size()-structure.highTail?" | selling tail":""));
        u.tip(u.id("tpo-price")+(uint64_t)s.day*8192+ri,hit,detail);
      }
    }

    auto level=[&](double price,const char* label,float width) {
      const float y=pane.yOf(price);
      if(y<area.y || y>area.y+area.h-16)return;
      const std::string_view name=label?label:"";
      const int rowIndex=name=="POC"?s.poc:name=="VAH"?s.vaHigh:name=="VAL"?s.vaLow:name=="PH"?(int)s.rows.size()-1:0;
      const auto& levelRow=s.rows[rowIndex];
      int last=s.lastPeriod; while(last>0 && !levelRow.periods.test(last)) --last;
      width=(split?last+1:levelRow.count())*cellWidth+6;
      for(float dx=0;dx<width;dx+=4)
        levelLines.push_back({{x+dx,y,2,1},withAlpha(label && (std::string_view(label)=="PH" || std::string_view(label)=="PL")?poorInk:name=="POC"?hexColor(0xffffff):t.textDim,.85f)});
      const int bit=name=="POC"?1:name=="VAH"?2:name=="VAL"?4:name=="PH"?8:16;
      if(label && (labelMask&bit)) allLabels.push_back({x+width+3,y-14,label,(bit>=8)?poorInk:t.text});
    };
    if(marks&1) level((s.rows[s.poc].tick+.5)*model.step,"POC",profileWidth+26);
    level((s.rows[s.vaHigh].tick+1)*model.step,"VAH",profileWidth+26);
    level(s.rows[s.vaLow].tick*model.step,"VAL",profileWidth+26);
    if(marks&8) {
      if(structure.poorHigh) level((s.rows.back().tick+1)*model.step,"PH",profileWidth+26);
      if(structure.poorLow) level(s.rows.front().tick*model.step,"PL",profileWidth+26);
    }
    using namespace std::chrono;
    const year_month_day date{sys_days{days{s.day}}};
    char dateLabel[24];
    snprintf(dateLabel,sizeof(dateLabel),"%02u/%02u",(unsigned)date.month(),(unsigned)date.day());
    if(s.days>1) snprintf(dateLabel,sizeof(dateLabel),"%02u/%02u +%dd",(unsigned)date.month(),(unsigned)date.day(),s.days-1);
    Rect select{x,area.y+area.h-16,spanWidth-4,16};
    if(u.hovered(select) && u.input.pressed) { selected=s.day; u.input.pressed=false; }
    if(dayWidth>60)u.draw.textAligned(select,dateLabel,selected==s.day?t.text:t.textDim,DrawList::Left);
    u.tip(u.id("tpo-select")+(uint64_t)s.day,select,"SPLIT shows 30-minute brackets in time order.");
  }
  // Draw level strokes after every box and block, using the line renderer for subpixel coverage.
  u.draw.breakCmd();
  for (const auto& line : levelLines)
    u.draw.line(line.rect.x,line.rect.y,line.rect.x+line.rect.w,line.rect.y,line.ink,1.0f);
  // Give POC/value-area labels priority, and never stack text from adjacent profiles.
  std::stable_sort(allLabels.begin(),allLabels.end(),[](const Label& a,const Label& b){
    auto rank=[](const char* s){return std::string_view(s)=="POC"?0:s[0]=='V'?1:2;};
    return rank(a.text)<rank(b.text);
  });
  static thread_local std::vector<Rect> occupied;
  occupied.clear();
  for(const auto& label:allLabels) {
    const float x=std::clamp(label.x,area.x,area.x+std::max(0.0f,area.w-34));
    for(float offset:{0.f,-14.f,14.f,-28.f,28.f}) {
      Rect r{x,label.y+offset,34,14};
      if(r.y<area.y || r.y+r.h>area.y+area.h-18) continue;
      bool clash=false;
      for(const Rect& other:occupied) if(r.x<other.x+other.w+3 && r.x+r.w+3>other.x && r.y<other.y+other.h && r.y+r.h>other.y) {clash=true;break;}
      if(clash) continue;
      u.draw.textAligned(r,label.text,label.ink,DrawList::Left);
      occupied.push_back(r); break;
    }
  }

}
