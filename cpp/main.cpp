#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using Day = long long;
Day currentDay() { return std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24; }

class LibraryError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
struct Book { int id; std::string title, author, category; int copies; };
struct Member { int id; std::string name, email; };
struct Loan { int id, bookId, memberId; Day issued, due, returned = -1; };
struct LibraryState { std::vector<Book> books; std::vector<Member> members; std::vector<Loan> loans; };

// Persistence is an interchangeable interface; business rules do not depend on files.
class Repository {
public:
    virtual LibraryState load() const = 0;
    virtual void save(const LibraryState&) const = 0;
    virtual ~Repository() = default;
};

class FileRepository final : public Repository {
    fs::path path;
public:
    explicit FileRepository(fs::path p) : path(std::move(p)) {}
    LibraryState load() const override {
        std::error_code ec;
        if (!fs::exists(path, ec)) { if (ec) throw LibraryError("Cannot inspect data file: " + ec.message()); return {}; }
        std::ifstream in(path); if (!in) throw LibraryError("Cannot open data file.");
        std::string line; if (!std::getline(in, line) || line != "FOLIO 1") throw LibraryError("Unsupported or corrupt file header.");
        LibraryState s; int number = 1;
        while (std::getline(in, line)) {
            ++number; std::istringstream row(line); char kind{}; row >> kind;
            bool valid = true;
            if (kind == 'B') { Book b{}; valid = bool(row >> b.id >> std::quoted(b.title) >> std::quoted(b.author) >> std::quoted(b.category) >> b.copies); if (valid) s.books.push_back(b); }
            else if (kind == 'M') { Member m{}; valid = bool(row >> m.id >> std::quoted(m.name) >> std::quoted(m.email)); if (valid) s.members.push_back(m); }
            else if (kind == 'L') { Loan l{}; valid = bool(row >> l.id >> l.bookId >> l.memberId >> l.issued >> l.due >> l.returned); if (valid) s.loans.push_back(l); }
            else valid = false;
            row >> std::ws;
            if (!valid || !row.eof()) throw LibraryError("Corrupt record at line " + std::to_string(number) + ". Original file preserved.");
        }
        if (!in.eof()) throw LibraryError("Data file read failed.");
        return s;
    }
    void save(const LibraryState& s) const override {
        auto tmp = path; tmp += ".tmp";
        auto backup = path; backup += ".bak";
        {
            std::ofstream out(tmp, std::ios::trunc); if (!out) throw LibraryError("Cannot create temporary data file.");
            out << "FOLIO 1\n";
            for (const auto& b : s.books) out << "B " << b.id << ' ' << std::quoted(b.title) << ' ' << std::quoted(b.author) << ' ' << std::quoted(b.category) << ' ' << b.copies << '\n';
            for (const auto& m : s.members) out << "M " << m.id << ' ' << std::quoted(m.name) << ' ' << std::quoted(m.email) << '\n';
            for (const auto& l : s.loans) out << "L " << l.id << ' ' << l.bookId << ' ' << l.memberId << ' ' << l.issued << ' ' << l.due << ' ' << l.returned << '\n';
            out.flush(); if (!out) throw LibraryError("Could not write data. Original file preserved.");
            out.close(); if (!out) throw LibraryError("Could not close data. Original file preserved.");
        }
        std::error_code ec;
        const bool exists = fs::exists(path);
        if (exists) { fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec); if (ec) throw LibraryError("Cannot create backup: " + ec.message()); }
        // C++17 rename replaces files on POSIX; Windows needs removal first.
        if (exists) { fs::remove(path, ec); if (ec) throw LibraryError("Cannot replace data: " + ec.message()); }
        fs::rename(tmp, path, ec);
        if (ec) {
            const auto reason = ec.message();
            if (exists) { std::error_code restore; fs::copy_file(backup, path, fs::copy_options::overwrite_existing, restore); if (restore) throw LibraryError("Save failed. Recover data from " + backup.string()); }
            throw LibraryError("Save failed: " + reason);
        }
    }
};

class Library {
    Repository& repo;
    LibraryState data;
    static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; }
    static void text(const std::string& s) { if (s.empty() || s.size()>2000 || std::any_of(s.begin(),s.end(),[](unsigned char c){return c<32 || c==127;}) || s.find_first_not_of(' ') == std::string::npos) throw LibraryError("Text cannot be empty, too long, or contain control characters."); }
    static bool validEmail(const std::string& email) { auto at=email.find('@'); return at!=std::string::npos && at>0 && at+1<email.size() && email.find('@',at+1)==std::string::npos && email.find('.',at+1)!=std::string::npos && email.find_first_of(" \t\r\n")==std::string::npos && email.back()!='.'; }
    template<class T> static int nextId(const std::vector<T>& rows) { int n = 0; for (const auto& r : rows) n = std::max(n, r.id); if (n == 2147483647) throw LibraryError("ID capacity reached."); return n + 1; }
    static void validate(const LibraryState& s) {
        std::unordered_map<int,int> books, members, loans;
        std::unordered_set<std::string> emails, pairs;
        for (const auto& b : s.books) { text(b.title); text(b.author); text(b.category); if (b.id < 1 || b.copies < 1 || b.copies>999 || !books.emplace(b.id,b.copies).second) throw LibraryError("Invalid or duplicate book."); }
        for (const auto& m : s.members) { text(m.name); text(m.email); if (m.id < 1 || !validEmail(m.email) || !emails.insert(lower(m.email)).second || !members.emplace(m.id,1).second) throw LibraryError("Invalid or duplicate member or email."); }
        for (const auto& l : s.loans) {
            if (l.id < 1 || !loans.emplace(l.id,1).second || !books.count(l.bookId) || !members.count(l.memberId) || l.issued < 0 || l.due < l.issued || (l.returned != -1 && l.returned < l.issued)) throw LibraryError("Invalid loan reference or date.");
            if (l.returned == -1) { if(!pairs.insert(std::to_string(l.bookId)+":"+std::to_string(l.memberId)).second)throw LibraryError("Duplicate active loan for a member and title.");if(--books[l.bookId] < 0) throw LibraryError("Active loans exceed book copies."); }
        }
    }
    template<class Fn> void transaction(Fn change) { auto next = data; change(next); validate(next); repo.save(next); data = std::move(next); }
public:
    explicit Library(Repository& r) : repo(r), data(r.load()) { validate(data); }
    const LibraryState& state() const { return data; }
    int available(int id) const { const auto it = std::find_if(data.books.begin(),data.books.end(),[=](const Book& b){return b.id==id;}); if(it==data.books.end())throw LibraryError("Book not found."); return it->copies - static_cast<int>(std::count_if(data.loans.begin(),data.loans.end(),[=](const Loan& l){return l.bookId==id && l.returned==-1;})); }
    int addBook(std::string title, std::string author, std::string category, int copies) { int id = nextId(data.books); transaction([&](auto& s){s.books.push_back({id,title,author,category,copies});}); return id; }
    int addMember(std::string name, std::string email) { if (email.find('@') == std::string::npos) throw LibraryError("Enter a valid email."); if (std::any_of(data.members.begin(),data.members.end(),[&](const auto& m){return lower(m.email)==lower(email);})) throw LibraryError("Email already registered."); int id = nextId(data.members); transaction([&](auto& s){s.members.push_back({id,name,email});}); return id; }
    std::vector<Book> search(const std::string& query) const { std::vector<Book> results; auto q = lower(query); std::copy_if(data.books.begin(),data.books.end(),std::back_inserter(results),[&](const Book& b){return lower(b.title+" "+b.author+" "+b.category).find(q)!=std::string::npos;}); std::sort(results.begin(),results.end(),[](const Book& a,const Book& b){return a.title<b.title;}); return results; }
    int issue(int bookId, int memberId, int days = 14) {
        if (days < 1 || days > 365) throw LibraryError("Loan period must be 1–365 days.");
        if (available(bookId) < 1) throw LibraryError("No copies available.");
        if (std::none_of(data.members.begin(),data.members.end(),[=](const Member& m){return m.id==memberId;})) throw LibraryError("Member not found.");
        if (std::any_of(data.loans.begin(),data.loans.end(),[=](const Loan& l){return l.bookId==bookId&&l.memberId==memberId&&l.returned==-1;})) throw LibraryError("Member already holds this title.");
        int id=nextId(data.loans); transaction([&](auto& s){s.loans.push_back({id,bookId,memberId,currentDay(),currentDay()+days,-1});}); return id;
    }
    void returnBook(int id) { transaction([=](auto& s){auto it=std::find_if(s.loans.begin(),s.loans.end(),[=](const Loan& l){return l.id==id;});if(it==s.loans.end())throw LibraryError("Loan not found.");if(it->returned!=-1)throw LibraryError("Book already returned.");it->returned=currentDay();}); }
    void renew(int id,int days=14) { if(days<1||days>365)throw LibraryError("Renewal period must be 1–365 days.");transaction([=](auto& s){auto it=std::find_if(s.loans.begin(),s.loans.end(),[=](const Loan& l){return l.id==id;});if(it==s.loans.end())throw LibraryError("Loan not found.");if(it->returned!=-1)throw LibraryError("Returned loans cannot be renewed.");if(it->due>currentDay()+365-days)throw LibraryError("Due date cannot exceed one year from today.");it->due=std::max(it->due,currentDay())+days;}); }
};

class MemoryRepository final : public Repository {
public: mutable LibraryState saved; mutable bool fail=false;
    LibraryState load() const override {return saved;}
    void save(const LibraryState& s) const override {if(fail)throw LibraryError("Simulated disk failure");saved=s;}
};
void require(bool condition, const std::string& msg) {if(!condition)throw std::runtime_error("Test failed: "+msg);}
template<class Fn> void rejects(Fn fn) {bool rejected=false;try{fn();}catch(const LibraryError&){rejected=true;}require(rejected,"expected validation error");}
void selfTest() {
    MemoryRepository repo; Library lib(repo);
    int b=lib.addBook("Atomic Habits","James Clear","Growth",1), m=lib.addMember("Reader","r@example.com"), m2=lib.addMember("Reader Two","r2@example.com");
    rejects([&]{lib.addBook("","x","y",1);}); rejects([&]{lib.addMember("Duplicate","R@example.com");});
    require(lib.search("HABITS").size()==1,"case-insensitive search");
    rejects([&]{lib.issue(b,999);}); rejects([&]{lib.issue(b,m,0);}); int l=lib.issue(b,m); require(lib.available(b)==0,"checkout decrements availability");
    rejects([&]{lib.issue(b,m2);}); lib.returnBook(l); require(lib.available(b)==1,"return restores availability"); rejects([&]{lib.returnBook(l);});
    rejects([&]{lib.renew(l);});rejects([&]{lib.addMember("Bad","bad@domain");});rejects([&]{lib.addBook("Too many","A","B",1000);});
    int renewed=lib.issue(b,m);const auto due=lib.state().loans.back().due;lib.renew(renewed);require(lib.state().loans.back().due==due+14,"renewal extends due date");require(lib.available(b)==0,"renewal preserves availability");lib.returnBook(renewed);
    auto corrupt=repo.saved;corrupt.members[1].email=corrupt.members[0].email;MemoryRepository duplicates;duplicates.saved=corrupt;rejects([&]{Library invalid(duplicates);});
    repo.fail=true; rejects([&]{lib.addBook("Unsaved","A","B",1);});require(lib.state().books.size()==1,"failed write rolls back in-memory state");repo.fail=false;
    const fs::path path=fs::temp_directory_path()/("folio-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".db");
    FileRepository disk(path);disk.save(lib.state());Library reloaded(disk);require(reloaded.state().loans.size()==2,"file round trip");disk.save(lib.state());require(fs::exists(path.string()+".bak"),"backup created on replacement");
    {std::ofstream bad(path);bad<<"FOLIO 1\nB broken\n";}rejects([&]{disk.load();});
    {std::ofstream bad(path);bad<<"FOLIO 1\n\n";}rejects([&]{disk.load();});
    std::error_code ec;fs::remove(path,ec);fs::remove(path.string()+".bak",ec);fs::remove(path.string()+".tmp",ec);
    std::cout<<"PASS: validation, availability, checkout/return/renewal, search, rollback, file round trip, backups, corruption detection.\n";
}

int main(int argc,char** argv) {
    try {
        if(argc>1&&std::string(argv[1])=="--self-test"){selfTest();return 0;}
        FileRepository repository(argc>1?argv[1]:"folio.db");Library library(repository);
        std::cout<<"\nFOLIO / Library engine\nCommands: books [query], members, loans, add-book \"title\" \"author\" \"category\" copies,\nadd-member \"name\" \"email\", issue book-id member-id [days], return loan-id, renew loan-id [days], quit\n";
        std::string line;
        while(std::cout<<"\nfolio> " && std::getline(std::cin,line)) {
            try {
                std::istringstream in(line);std::string cmd;in>>cmd;if(cmd.empty())continue;if(cmd=="quit")break;
                auto end=[&]{in>>std::ws;if(!in.eof())throw LibraryError("Unexpected extra arguments.");};
                if(cmd=="books"){std::string q;std::getline(in,q);if(!q.empty()&&q[0]==' ')q.erase(0,1);for(const auto& b:library.search(q))std::cout<<b.id<<" | "<<b.title<<" / "<<b.author<<" | "<<library.available(b.id)<<'/'<<b.copies<<" available\n";}
                else if(cmd=="members"){end();for(const auto& m:library.state().members)std::cout<<m.id<<" | "<<m.name<<" | "<<m.email<<'\n';}
                else if(cmd=="loans"){end();for(const auto& l:library.state().loans)std::cout<<l.id<<" | book "<<l.bookId<<" | member "<<l.memberId<<" | "<<(l.returned!=-1?"Returned":l.due<currentDay()?"Overdue":"On loan")<<" | due in "<<l.due-currentDay()<<" days\n";}
                else if(cmd=="add-book"){std::string t,a,c;int n;if(!(in>>std::quoted(t)>>std::quoted(a)>>std::quoted(c)>>n))throw LibraryError("Expected quoted title, author, category, and copy count.");end();std::cout<<"Added book "<<library.addBook(t,a,c,n)<<'\n';}
                else if(cmd=="add-member"){std::string n,e;if(!(in>>std::quoted(n)>>std::quoted(e)))throw LibraryError("Expected quoted name and email.");end();std::cout<<"Added member "<<library.addMember(n,e)<<'\n';}
                else if(cmd=="issue"){int b,m,d=14;if(!(in>>b>>m))throw LibraryError("Expected book ID and member ID.");in>>std::ws;if(!in.eof()&&!(in>>d))throw LibraryError("Expected loan period in days.");end();std::cout<<"Created loan "<<library.issue(b,m,d)<<'\n';}
                else if(cmd=="return"){int id;if(!(in>>id))throw LibraryError("Expected loan ID.");end();library.returnBook(id);std::cout<<"Return recorded.\n";}
                else if(cmd=="renew"){int id,d=14;if(!(in>>id))throw LibraryError("Expected loan ID.");in>>std::ws;if(!in.eof()&&!(in>>d))throw LibraryError("Expected renewal period in days.");end();library.renew(id,d);std::cout<<"Loan renewed.\n";}
                else throw LibraryError("Unknown command. Use books, members, loans, add-book, add-member, issue, return, renew, or quit.");
            }catch(const std::exception& e){std::cerr<<"Error: "<<e.what()<<'\n';}
        }
    }catch(const std::exception& e){std::cerr<<"Folio could not start: "<<e.what()<<'\n';return 1;}
}
