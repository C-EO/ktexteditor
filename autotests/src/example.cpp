#include <KParts/MainWindow>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/View>

#include <QApplication>
#include <QToolBar>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    KParts::MainWindow *m = new KParts::MainWindow();

    auto e = KTextEditor::Editor::instance();
    auto doc = e->createDocument(nullptr);
    doc->openUrl(QUrl("file:///home/waqar/kde/src/kde/applications/kate/kate/kateapp.cpp"));
    //    doc->openUrl(QUrl("file:///home/waqar/large.txt"));
    //    doc->openUrl(QUrl("file:///home/waqar/test.cpp"));
    //    doc->setText(QStringLiteral("void fn(int a,)"));

    auto v = doc->createView(m);
    v->setCursorPosition(KTextEditor::Cursor(143, 54));

    QToolBar tb(m);
    tb.addAction("Config...", m, [e, m] {
        e->configDialog(m);
    });

    m->addToolBar(&tb);

    m->setCentralWidget(v);
    m->setFixedSize(400, 400);
    m->show();

    return app.exec();
}
