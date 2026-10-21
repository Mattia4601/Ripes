#include "remotefiledialog.h"
#include "remotefileengine.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace Ripes {

namespace {

// "Source files [*.s, *.c] (*.s *.c);; All files (*)" -> {"*.s", "*.c"}
QStringList nameFiltersFrom(const QString &filter) {
  const QString first = filter.section(QStringLiteral(";;"), 0, 0);
  const auto match =
      QRegularExpression(QStringLiteral("\\(([^)]*)\\)")).match(first);
  QStringList patterns =
      (match.hasMatch() ? match.captured(1) : first)
          .split(u' ', Qt::SkipEmptyParts);
  if (patterns.isEmpty())
    patterns << QStringLiteral("*");
  return patterns;
}

} // namespace

RemoteFileDialog::RemoteFileDialog(Mode mode, const QString &title,
                                   const QString &filter, QWidget *parent)
    : QDialog(parent), m_mode(mode), m_nameFilters(nameFiltersFrom(filter)) {
  setWindowTitle(title);

  m_fileList = new QListWidget(this);
  m_fileName = new QLineEdit(this);
  m_buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  m_buttons->button(QDialogButtonBox::Ok)
      ->setText(mode == Mode::Open ? tr("Open") : tr("Save"));

  auto *form = new QFormLayout;
  form->addRow(tr("File name:"), m_fileName);

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(new QLabel(
      tr("Files on the server (%1)").arg(m_nameFilters.join(u' ')), this));
  layout->addWidget(m_fileList);
  layout->addLayout(form);
  layout->addWidget(m_buttons);

  connect(m_fileList, &QListWidget::currentTextChanged, m_fileName,
          &QLineEdit::setText);
  connect(m_fileList, &QListWidget::itemDoubleClicked, this,
          &RemoteFileDialog::accept);
  connect(m_fileName, &QLineEdit::textChanged, this,
          &RemoteFileDialog::updateOkButton);
  connect(m_buttons, &QDialogButtonBox::accepted, this,
          &RemoteFileDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  refresh();
  updateOkButton();
}

void RemoteFileDialog::refresh() {
  m_entries = QDir(RemoteFileSystemPrefix + u'/')
                  .entryList(m_nameFilters, QDir::Files, QDir::Name);
  m_fileList->clear();
  m_fileList->addItems(m_entries);
}

void RemoteFileDialog::updateOkButton() {
  const QString name = m_fileName->text().trimmed();
  bool valid = !name.isEmpty() && !name.contains(u'/');
  if (m_mode == Mode::Open)
    valid &= m_entries.contains(name);
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

void RemoteFileDialog::accept() {
  if (!m_buttons->button(QDialogButtonBox::Ok)->isEnabled())
    return;
  emit fileSelected(RemoteFileSystemPrefix + u'/' +
                    m_fileName->text().trimmed());
  QDialog::accept();
}

} // namespace Ripes
