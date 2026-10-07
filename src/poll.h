#pragma once
#include <jansson.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

struct PollData {
    struct Option { std::string text; int votes=0, percent=0; bool chosen=false, pending=false, correct=false; };
    int64_t id=0;
    std::string question, explanation;
    std::vector<Option> options;
    std::vector<int> order;
    int voters=0;
    bool multiple=false, quiz=false, anonymous=true, closed=false, revote=false, restricted=false, results=false;
    static std::string text(json_t* o) {
        if (json_is_object(o)) o=json_object_get(o,"text");
        const char* s=json_string_value(o); return s ? s : "";
    }
    static PollData parse(json_t* p) {
        PollData out;
        json_t* id=json_object_get(p,"id");
        out.id=json_is_string(id) ? std::strtoll(json_string_value(id),nullptr,10) : json_integer_value(id);
        out.question=text(json_object_get(p,"question"));
        json_t* type=json_object_get(p,"type");
        out.quiz=text(json_object_get(type,"@type"))=="pollTypeQuiz";
        auto flag=[&](const char* name) { return json_is_true(json_object_get(p,name)); };
        out.multiple=flag("allows_multiple_answers") || json_is_true(json_object_get(type,"allow_multiple_answers"));
        out.anonymous=flag("is_anonymous"); out.closed=flag("is_closed"); out.results=flag("can_see_results");
        out.revote=json_object_get(p,"allows_revoting") ? flag("allows_revoting") : !out.quiz;
        out.restricted=json_is_object(json_object_get(p,"vote_restriction_reason"));
        out.explanation=text(json_object_get(type,"explanation"));
        out.voters=(int)json_integer_value(json_object_get(p,"total_voter_count"));
        size_t n; json_t* v;
        json_array_foreach(json_object_get(p,"options"),n,v) {
            Option o; o.text=text(json_object_get(v,"text"));
            o.votes=(int)json_integer_value(json_object_get(v,"voter_count"));
            o.percent=std::max(0,std::min(100,(int)json_integer_value(json_object_get(v,"vote_percentage"))));
            o.chosen=json_is_true(json_object_get(v,"is_chosen")); o.pending=json_is_true(json_object_get(v,"is_being_chosen"));
            out.options.push_back(o);
        }
        json_array_foreach(json_object_get(type,"correct_option_ids"),n,v) {
            int i=(int)json_integer_value(v); if (i>=0 && i<(int)out.options.size()) out.options[i].correct=true;
        }
        json_t* old=json_object_get(type,"correct_option_id");
        if (json_is_integer(old)) { int i=(int)json_integer_value(old); if (i>=0 && i<(int)out.options.size()) out.options[i].correct=true; }
        json_array_foreach(json_object_get(p,"option_order"),n,v) {
            int i=(int)json_integer_value(v);
            if (i>=0 && i<(int)out.options.size() && std::find(out.order.begin(),out.order.end(),i)==out.order.end()) out.order.push_back(i);
        }
        for (int i=0;i<(int)out.options.size();++i)
            if (std::find(out.order.begin(),out.order.end(),i)==out.order.end()) out.order.push_back(i);
        return out;
    }
    bool answered() const { for (const auto& o:options) if (o.chosen) return true; return false; }
    bool canVote() const {
        if (!id || closed || restricted || (!revote && answered())) return false;
        for (const auto& o:options) if (o.pending) return false;
        return true;
    }
    bool validAnswer(const std::vector<int>& answer) const {
        if (!canVote() || (!multiple && answer.size()>1)) return false;
        if (answer.empty()) return !quiz && revote && answered();
        std::vector<int> seen;
        for (int i:answer) {
            if (i<0 || i>=(int)options.size() || std::find(seen.begin(),seen.end(),i)!=seen.end()) return false;
            seen.push_back(i);
        }
        return true;
    }
};
