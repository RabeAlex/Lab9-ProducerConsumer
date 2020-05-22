// Copyright 2018 Your Name <your_email>

#include <header.hpp>

class loader {
public:
    loader(boost::asio::io_context& ioc_,
    std::vector<std::string>& url_, size_t& depth_,
    size_t& network_threads_, size_t& parser_threads_, std::string& FileName_)
        : ioc(ioc_), LinkVector{ url_ }, depth(depth_),
        network_threads(network_threads_),
        parser_threads(parser_threads_), FileName(FileName_)
    {}

    void handler() {
        std::vector<std::vector<std::string>> ParsPagesLinks;
        std::vector<std::vector<std::string>> ParsPagesImage;
        while (depth--) {
            std::vector<std::string> SelectedLinkVector;
            // in cases when Network is unreachable
            PCqueue.clear();
            ParsPagesImage.clear();
            ParsPagesLinks.clear();
            ParsPagesLinks.reserve(LinkVector.size());
            ParsPagesImage.reserve(LinkVector.size());

            boost::asio::thread_pool ProducerPool{network_threads};
            for (auto const &LinkString : LinkVector) {
                boost::asio::post(ProducerPool, std::bind(&loader::load, this,
                std::cref(LinkString), std::ref(SelectedLinkVector)));
            }
            ProducerPool.join();

            boost::asio::thread_pool ConsumerPool{parser_threads};
            boost::asio::post(ConsumerPool, std::bind(
            &loader::ParsPreparation, this,
            std::ref(ParsPagesLinks), std::ref(ParsPagesImage)));
            ConsumerPool.join();

            for (size_t a = 0; a < SelectedLinkVector.size(); ++a) {
                for (size_t b = 0; b < ParsPagesLinks.size(); ++b) {
                    CorrectLinks(SelectedLinkVector.at(
                            SelectedLinkVector.size() - 1 - a),
                            ParsPagesLinks.at(b));
                    CorrectLinks(SelectedLinkVector.at(
                            SelectedLinkVector.size() - 1 - a),
                            ParsPagesImage.at(b));
                }
            }

            print(FileName, SelectedLinkVector, ParsPagesLinks, ParsPagesImage);

            LinkVector.clear();
            for (auto const &vct : ParsPagesLinks) {
                LinkVector.insert(LinkVector.end(), vct.begin(), vct.end());
            }
        }
    }

    static void print(std::string& FileName,
    std::vector<std::string>& SelectedLinkVector,
    std::vector<std::vector<std::string>>& ParsPagesLinks,
    std::vector<std::vector<std::string>>& ParsPagesImage) {
        std::ofstream out(FileName, std::ios::in | std::ios::binary);
        if (!out) {
            std::cout << "File " << FileName << " not found!\n";
            exit(EXIT_FAILURE);
        }
        out << "\n******************************"
            << " Список из " << SelectedLinkVector.size()
            << " просмотренных страниц: ******************************\n\n";
        for (size_t i = 0; i < ParsPagesLinks.size(); ++i) {
            out << "Ссылок: " << std::setw(5) << std::left
                << ParsPagesLinks.at(i).size() << " для страницы: "
                << SelectedLinkVector.at(SelectedLinkVector.size() - 1 - i)
                << '\n';
            out << "Изображений: " << std::setw(5) << std::left
                << ParsPagesImage.at(i).size() << "\n";
            for (auto const &str : ParsPagesImage.at(i)) {
                out << str << '\n';
            }
            out << "\n";
        }
    }

    void ParsPreparation(
            std::vector<std::vector<std::string>>& PPL,
            std::vector<std::vector<std::string>>& PPI) {
        int QueueSize = PCqueue.size();
        while (QueueSize) {
            PPL.emplace_back();
            PPI.emplace_back();
            FindLinks(PPL.back(), PPI.back());
            DuplicateRemoval(PPI.back());
            DuplicateRemoval(PPL.back());
            --QueueSize;
        }
    }

    static void DuplicateRemoval(std::vector<std::string>& ParsPages) {
        std::set<std::string> NoDuplicate(ParsPages.begin(), ParsPages.end());
        ParsPages.clear();
        for (auto const& element : NoDuplicate) {
            ParsPages.emplace_back(element);
        }
    }

    void FindLinks(std::vector<std::string>& PPL,
                   std::vector<std::string>& PPI) {
        GumboOutput *output = gumbo_parse(PCqueue.back()->c_str());
        PCqueue.pop_back();
        std::queue<GumboNode *> queue;
        queue.push(output->root);
        while (!queue.empty()) {
            GumboNode *node = queue.front();
            queue.pop();
            if (GUMBO_NODE_ELEMENT == node->type) {
                GumboAttribute *href = nullptr;
                if ((node->v.element.tag == GUMBO_TAG_BASE ||
                node->v.element.tag == GUMBO_TAG_A) &&
                (href = gumbo_get_attribute(&node->v.element.attributes,
                        "href"))) {
                    PPL.emplace_back(href->value);
                }
                GumboAttribute *img = nullptr;
                if (node->v.element.tag == GUMBO_TAG_IMG &&
                (img = gumbo_get_attribute(&node->v.element.attributes,
                        "src"))) {
                    PPI.emplace_back(img->value);
                }
                GumboVector *children = &node->v.element.children;
                for (unsigned int i = 0; i < children->length; ++i) {
                    queue.push(static_cast<GumboNode *>(children->data[i]));
                }
            }
        }
        gumbo_destroy_output(&kGumboDefaultOptions, output);
    }

    void load(std::string const& LinkString,
            std::vector<std::string>& SLV) {
        std::smatch parts;
        if (std::regex_match(LinkString, parts, RegularSelector)) {
            if (parts[1].str() == "https") {
                LoadHttps(parts, SLV);
            } else {
                LoadHttp(parts, SLV);
            }
        } else {
            {
                std::lock_guard<std::mutex> lg{ mtx };
                std::cerr << "load() std::regex_match() failed: " +
                LinkString << "\n\n";
            }
        }
    }

    void LoadHttp(std::smatch const& parts,
            std::vector<std::string>& SLV)
    {
        try {
            std::string host = parts[2];
            std::string const target = (
                    parts[3].length() == 0 ? "/" : parts[3].str());
            int version = 11;
            boost::asio::ip::tcp::socket socket{ ioc };
            boost::asio::ip::tcp::resolver resolver{ ioc };
            auto const results = resolver.resolve(host, "80");
            boost::asio::connect(socket, results.begin(), results.end());
            boost::beast::http::request<boost::beast::http::string_body> req{
                boost::beast::http::verb::get, target, version };
            req.set(boost::beast::http::field::host, parts[2]);
            req.set(boost::beast::http::field::user_agent,
                    BOOST_BEAST_VERSION_STRING);
            boost::beast::http::write(socket, req);
            boost::beast::flat_buffer buffer;
            boost::beast::http::response<boost::beast::http::dynamic_body> res;
            boost::beast::http::read(socket, buffer, res);

            std::string PageBody =
                    boost::beast::buffers_to_string(res.body().data());
            PCqueue.push_back(new std::string(PageBody));
            SLV.push_back(parts[0]);

            boost::system::error_code ec;
            socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            if (ec && ec != boost::system::errc::not_connected) {
                throw boost::system::system_error{ ec };
            }
        } catch (std::exception const& e) {
            {
                std::lock_guard<std::mutex> lg{ mtx };
                std::cerr << "loadHttp(): " << e.what() << std::endl;
            }
        }
    }

    void LoadHttps(std::smatch const& parts,
            std::vector<std::string>& SLV)
    {
        try {
            std::string host = parts[2];
            std::string const target = (
                    parts[3].length() == 0 ? "/" : parts[3].str());
            int version = 11;
            ssl::context ctx{ ssl::context::sslv23_client };
            load_root_certificates(ctx);
            ssl::stream<boost::asio::ip::tcp::socket> stream{ ioc, ctx };
            if (!SSL_set_tlsext_host_name(
                    stream.native_handle(), host.c_str())) {
                boost::system::error_code ec{
                    static_cast<int>(::ERR_get_error()),
                    boost::asio::error::get_ssl_category() };
                throw boost::system::system_error{ ec };
            }
            boost::asio::ip::tcp::resolver resolver{ ioc };
            auto const results = resolver.resolve(host, "443");
            boost::asio::connect(stream.next_layer(),
                    results.begin(), results.end());
            stream.handshake(ssl::stream_base::client);
            boost::beast::http::request<boost::beast::http::string_body> req{
                boost::beast::http::verb::get, target, version };
            req.set(boost::beast::http::field::host, host);
            req.set(boost::beast::http::field::user_agent,
                    BOOST_BEAST_VERSION_STRING);
            boost::beast::http::write(stream, req);
            boost::beast::flat_buffer buffer;
            boost::beast::http::response<boost::beast::http::dynamic_body> res;
            boost::beast::http::read(stream, buffer, res);

            std::string PageBody =
                    boost::beast::buffers_to_string(res.body().data());
            PCqueue.push_back(new std::string(PageBody));
            SLV.push_back(parts[0]);

            boost::system::error_code ec;
            stream.shutdown(ec);
            if (ec == boost::asio::error::eof) {
                ec.assign(0, ec.category());
            }
            if (ec) {
                throw boost::system::system_error{ec};
            }
        } catch (std::exception const& e) {
            {
                std::lock_guard<std::mutex> lg{ mtx };
                std::cerr << "loadHttps(): " << e.what() << std::endl;
            }
        }
    }

    void CorrectLinks(std::string const& SelectedLink,
        std::vector<std::string>& ParsPages) {
        std::smatch parts;
        std::regex_match(SelectedLink, parts, RegularSelector);
        std::string reference = parts[0];
        std::string protocol = parts[1];
        std::string host = parts[2];

        for (auto & str : ParsPages) {
            if (str.find("//") == 0) {
                str = protocol + ":" + str;
            } else if (str.find("/") == 0) {
                str = protocol + "://" + host + str;
            } else if (str.find("http") != 0 || str.find("https") != 0) {
                std::string r = reference;
                int count = 0;
                while (true) {
                    if (str.find("../") != 0) {
                        break;
                    }
                    ++count;
                    str.erase(0, 3);
                }
                while (count) {
                    r.pop_back();
                    while (r.back() != '/') {
                        r.pop_back();
                    }
                    --count;
                }
                str = r + str;
            }
        }
    }

    loader(loader const&) = delete;
    loader& operator = (loader const&) = delete;

private:
    std::regex RegularSelector{ "^(?:(https?)://)([^/]+)(/.*)?" };
    std::mutex mtx;
    boost::asio::io_context& ioc;
    std::vector<std::string> LinkVector;
    size_t depth;
    size_t network_threads;
    size_t parser_threads;
    std::string FileName;
        std::vector<std::string*> PCqueue;
};

int main(int argc, char** argv) {
    boost::program_options::options_description desc("caption");
    desc.add_options()
            ("url", boost::program_options::value<std::string>())
            ("depth", boost::program_options::value<size_t>())
            ("network_threads", boost::program_options::value<size_t>())
            ("parser_threads", boost::program_options::value<size_t>())
            ("output", boost::program_options::value<std::string>());
    boost::program_options::variables_map vm;
    try {
        boost::program_options::store(boost::program_options::
                                      parse_command_line(argc, argv, desc), vm);
        boost::program_options::notify(vm);
    }
    catch (boost::program_options::error &e) {
        std::cout << e.what() << std::endl;
    }
    std::vector<std::string> url { vm["url"].as<std::string>() };
    size_t depth = vm["depth"].as<size_t>();
    size_t network_threads = vm["network_threads"].as<size_t>();
    size_t parser_threads = vm["parser_threads"].as<size_t>();
    std::string FileName = vm["output"].as<std::string>();
    boost::asio::io_context ioc;

    loader ldr { ioc , url, depth, network_threads, parser_threads, FileName};
    ldr.handler();
}

